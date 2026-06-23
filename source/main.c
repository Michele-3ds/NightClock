#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

#define STAR_COUNT       80
#define SHOOT_MAX         12  
#define CLOUD_COUNT       7
#define EMBER_COUNT       30  

#define PLANE_TRAIL_MAX  32
#define COMET_TRAIL_MAX  64

#define SAVE_PATH_RECORDS  "sdmc:/3ds/NightClock/records.bin"
#define SAVE_PATH_SETTINGS "sdmc:/3ds/NightClock/settings.bin"
#define SAVE_DIR           "sdmc:/3ds/NightClock"

#define THEME_TRANSITION_SPEED 0.05f
#define COLOR_TRANSITION_SPEED 0.08f 

#define COMPILER_COLOR(r,g,b,a) (((u32)(a)<<24)|((u32)(b)<<16)|((u32)(g)<<8)|(u32)(r))

/* -------------------- STRUCTS & ENUMS -------------------- */
typedef enum {
    SCREEN_MAIN,
    SCREEN_SETTINGS,
    SCREEN_RECORDS,
    SCREEN_CREDITS      
} MenuScreen;

typedef struct {
    u32 shootingStars;
    u32 planes;
    u32 clouds;
    u32 totalTimeSeconds;
} AppRecords;

typedef struct {
    u32 clockColorIndex;
    u32 timeFormat24h;
    u32 dateFormat; // 0 = US, 1 = EU, 2 = ISO
    u32 bgThemeIndex;   
    u32 clockSizePreset; 
    u32 clockMode;       
} AppSettings;

typedef struct {
    float x, y;
    float phase;
    float speed;
    float base;
    float size;
    float pulseFreq;
    float pulseAmp;
} Star;

typedef struct {
    float x, y;
    float vx, vy;
    float life;
    float maxLife;
    int active;
} Shoot;

typedef struct {
    float x, y;
    float speed;
    float scale;
    float layer;
    float wobble;
} Cloud;

typedef struct {
    float x, y;
    float alpha;
} TrailPoint;

typedef struct {
    float x, y;
    float speed;
    int   active;       
    float direction;    
    TrailPoint trail[PLANE_TRAIL_MAX];
    int head;           
    int count;          
    int trailTimer;
} Airplane;

typedef struct {
    float startX, startY;
    float endX,   endY;
    float progress;
    float speed;
    float arcHeight;
    int   active;
    TrailPoint trail[COMET_TRAIL_MAX];
    int head;           
    int count;          
    int trailTimer;
} Comet;

typedef struct {
    float x, y;
    float speedY;
    float wobbleSpeed;
    float wobblePhase;
    float wobbleAmp;
    float alpha;
    float size;
} Ember;

typedef struct {
    float topR, topG, topB;
    float botR, botG, botB;
    u32 textMenuColor;
    u32 emberColor; 
    const char* name;
} SkyTheme;

/* -------------------- GLOBAL VARIABLES / CONSTANTS -------------------- */
static const SkyTheme skyThemes[5] = {
    {5.0f,  8.0f,  18.0f, 25.0f,  36.0f, 78.0f, COMPILER_COLOR(65,  140, 245, 255), COMPILER_COLOR(245, 130, 35,  255), "Midnight Blue"},     
    {15.0f, 5.0f,  30.0f, 85.0f,  15.0f, 65.0f, COMPILER_COLOR(230, 60,  180, 255), COMPILER_COLOR(210, 50,  230, 255), "Cyberpunk Purple"},  
    {0.0f,  0.0f,  2.0f,  10.0f,  12.0f, 20.0f, COMPILER_COLOR(130, 140, 160, 255), COMPILER_COLOR(140, 160, 190, 255), "Deep Space Black"},   
    {4.0f,  15.0f, 12.0f, 10.0f,  75.0f, 45.0f, COMPILER_COLOR(40,  210, 130, 255), COMPILER_COLOR(90,  240, 150, 255), "Aurora Borealis"},   
    {2.0f,  0.0f,  0.0f,  50.0f,  4.0f,  4.0f,  COMPILER_COLOR(230, 30,  30,  255), COMPILER_COLOR(255, 40,  40,  255), "Blood Moon"}         
};

static Star     stars[STAR_COUNT];
static Shoot    shoots[SHOOT_MAX];
static Cloud    clouds[CLOUD_COUNT];
static Ember    embers[EMBER_COUNT]; 
static Airplane planeA;
static Airplane planeB;
static Comet    hourlyComet;

static int planeATimer = 1800;
static int planeBTimer = 1200;
static int shootTimer  = 300;

static int halfHourStarsToSpawn = 0;
static int halfHourSpawnDelayTimer = 0;

static float currentTopR = 5.0f,  currentTopG = 8.0f,  currentTopB = 18.0f;
static float currentBotR = 25.0f, currentBotG = 36.0f, currentBotB = 78.0f;
static float currentClockR = 255.0f, currentClockG = 255.0f, currentClockB = 255.0f;
static float currentThemeTextR = 65.0f, currentThemeTextG = 140.0f, currentThemeTextB = 245.0f;

static bool settingsDirty = false;

static MenuScreen currentScreen = SCREEN_MAIN;
static AppRecords records  = {0, 0, 0, 0};
static AppSettings settings = {0, 1, 1, 0, 0, 0}; 
static u32 sessionFrames = 0;

static bool lowerScreenOff = false;

static C2D_TextBuf staticBuf;
typedef struct {
    C2D_Text title, hintSelect, hintSize, hintStart, hintOff, btnSettings, btnRecords, btnOff;
    C2D_Text recTitle, btnResetStats, btnBack;
    C2D_Text setTitle, btnBackSet, btnResetSet, btnCredits;
    C2D_Text credTitle, credLine1, credLine2, credLine3, credLine4, btnBackCred;
} StaticTexts;
static StaticTexts ui;

static u32 clockPresets[10];
static const char* presetNames[11] = {
    "Pure White", "Neon Blue",    "Emerald Green", "Blood Moon Red", "Cyber Yellow",
    "Hot Pink",   "Soft Cyan",    "Amethyst",      "Orange Fox",     "Mint Green",
    "Rainbow Dreams"
};

static void initColorPresets() {
    clockPresets[0] = C2D_Color32(255, 255, 255, 255);
    clockPresets[1] = C2D_Color32(  0, 191, 255, 255);
    clockPresets[2] = C2D_Color32( 50, 205,  50, 255);
    clockPresets[3] = C2D_Color32(230,  30,  30, 255);
    clockPresets[4] = C2D_Color32(255, 215,   0, 255);
    clockPresets[5] = C2D_Color32(255,  20, 147, 255);
    clockPresets[6] = C2D_Color32(128, 255, 212, 255);
    clockPresets[7] = C2D_Color32(153,  50, 204, 255);
    clockPresets[8] = C2D_Color32(255,  69,   0, 255);
    clockPresets[9] = C2D_Color32(173, 255,  47, 255);
}

static void initStaticTexts() {
    staticBuf = C2D_TextBufNew(1024);
    #define PT(dst, str) C2D_TextParse(dst, staticBuf, str); C2D_TextOptimize(dst)
    // Home Screen
    PT(&ui.title,       "--- NIGHTCLOCK ---");
    PT(&ui.hintSelect,  "Press SELECT to Cycle Display Modes");
    PT(&ui.hintSize,    "Press D-Pad Left/Right to Resize Clock");
    PT(&ui.hintStart,   "Press START to Close Application");
    PT(&ui.hintOff,     "Tap 'Screen Off' to Disable the Lower Screen");
    PT(&ui.btnSettings, "Settings");
    PT(&ui.btnRecords,  "Records");
    PT(&ui.btnOff,      "Screen Off");
    
    // Records Screen
    PT(&ui.recTitle,    "NIGHTCLOCK RECORDS");
    PT(&ui.btnResetStats, "Reset Stats");
    PT(&ui.btnBack,     "Back");
    
    // Settings Screen
    PT(&ui.setTitle,    "SETTINGS");
    PT(&ui.btnBackSet,  "Back");
    PT(&ui.btnResetSet, "Reset");
    PT(&ui.btnCredits,  "Credits");

    // Credits Screen
    PT(&ui.credTitle,   "NIGHTCLOCK CREDITS");
    PT(&ui.credLine1,   "Developed by: Michele P.");
    PT(&ui.credLine2,   "A Relaxing NightClock App");
    PT(&ui.credLine3,   "Send feedbacks to Michelep3ds@gmail.com");
    PT(&ui.credLine4,   "Version 1.0");
    PT(&ui.btnBackCred, "Back");
    #undef PT
}

/* -------------------- SAVE / LOAD SYSTEM -------------------- */
static void saveRecords() {
    FILE* f = fopen(SAVE_PATH_RECORDS, "wb");
    if (f) { fwrite(&records, sizeof(AppRecords), 1, f); fclose(f); }
}

static void loadRecords() {
    FILE* f = fopen(SAVE_PATH_RECORDS, "rb");
    if (f) { fread(&records, sizeof(AppRecords), 1, f); fclose(f); }
    else   { records = (AppRecords){0, 0, 0, 0}; }
}

static void saveSettings() {
    FILE* f = fopen(SAVE_PATH_SETTINGS, "wb");
    if (f) { fwrite(&settings, sizeof(AppSettings), 1, f); fclose(f); }
    settingsDirty = false;
}

static void loadSettings() {
    FILE* f = fopen(SAVE_PATH_SETTINGS, "rb");
    if (f) { fread(&settings, sizeof(AppSettings), 1, f); fclose(f); }
    else   { settings = (AppSettings){0, 1, 1, 0, 0, 0}; }
}

/* -------------------- INITIALIZATION -------------------- */
static void initPlaneInsideStruct(Airplane* p, float dir) {
    p->active     = 0;
    p->head       = 0;
    p->count      = 0;
    p->trailTimer = 0;
    p->direction  = dir;
}

static void initSingleEmber(int i, bool randomY) {
    embers[i].x            = rand() % 400;
    embers[i].y            = randomY ? (145 + rand() % 95) : 240.0f; 
    embers[i].speedY       = 0.3f + (rand() % 100) / 150.0f;
    embers[i].wobbleSpeed  = 0.03f + (rand() % 100) / 2000.0f;
    embers[i].wobblePhase  = rand() % 360;
    embers[i].wobbleAmp    = 0.5f + (rand() % 100) / 80.0f;
    embers[i].alpha        = 0.4f + (rand() % 100) / 200.0f;
    embers[i].size         = 1.0f + (rand() % 100) / 80.0f;
}

static void initStars() {
    srand(time(NULL));
    initColorPresets();

    for (int i = 0; i < STAR_COUNT; i++) {
        stars[i].x         = rand() % 400;
        stars[i].y         = rand() % 240;
        stars[i].phase     = rand() % 360;
        stars[i].speed     = 0.2f  + (rand() % 100) / 400.0f;
        stars[i].base      = 0.3f  + (rand() % 70)  / 100.0f;
        stars[i].size      = 0.6f  + (rand() % 100) / 200.0f;
        stars[i].pulseFreq = 0.5f  + (rand() % 100) / 100.0f;
        stars[i].pulseAmp  = 0.2f  + (rand() % 80)  / 100.0f;
    }

    for (int i = 0; i < SHOOT_MAX; i++) shoots[i].active = 0;

    for (int i = 0; i < CLOUD_COUNT; i++) {
        clouds[i].x      = rand() % 400;
        clouds[i].y      = 20 + rand() % 80;
        clouds[i].layer  = (float)i / CLOUD_COUNT;
        clouds[i].scale  = 0.5f + clouds[i].layer * 0.8f;
        clouds[i].speed  = 0.05f + (clouds[i].scale * 0.12f);
        clouds[i].wobble = rand() % 360;
    }

    for (int i = 0; i < EMBER_COUNT; i++) {
        initSingleEmber(i, true);
    }

    initPlaneInsideStruct(&planeA, +1.0f);
    initPlaneInsideStruct(&planeB, -1.0f);
    hourlyComet.active = 0;
    hourlyComet.head   = 0;
    hourlyComet.count  = 0;
    hourlyComet.trailTimer = 0;

    u32 activeTheme = (settings.bgThemeIndex < 5) ? settings.bgThemeIndex : 0;
    currentTopR = skyThemes[activeTheme].topR;
    currentTopG = skyThemes[activeTheme].topG;
    currentTopB = skyThemes[activeTheme].topB;
    currentBotR = skyThemes[activeTheme].botR;
    currentBotG = skyThemes[activeTheme].botG;
    currentBotB = skyThemes[activeTheme].botB;

    u32 themeCol = skyThemes[activeTheme].textMenuColor;
    currentThemeTextR = (float)(themeCol & 0xFF);
    currentThemeTextG = (float)((themeCol >> 8) & 0xFF);
    currentThemeTextB = (float)((themeCol >> 16) & 0xFF);

    if (settings.clockColorIndex < 10) {
        u32 targetCol = clockPresets[settings.clockColorIndex];
        currentClockR = (float)(targetCol & 0xFF);
        currentClockG = (float)((targetCol >> 8) & 0xFF);
        currentClockB = (float)((targetCol >> 16) & 0xFF);
    }
}

/* -------------------- EMBERS SYSTEM -------------------- */
static void updateAndDrawEmbers(float t) {
    u32 activeTheme = (settings.bgThemeIndex < 5) ? settings.bgThemeIndex : 0;
    u32 themeColor  = skyThemes[activeTheme].emberColor;

    u8 r = (u8)(themeColor & 0xFF);
    u8 g = (u8)((themeColor >> 8) & 0xFF);
    u8 b = (u8)((themeColor >> 16) & 0xFF);

    for (int i = 0; i < EMBER_COUNT; i++) {
        embers[i].y -= embers[i].speedY;
        embers[i].x += sinf(t * embers[i].wobbleSpeed + embers[i].wobblePhase) * embers[i].wobbleAmp * 0.3f;

        float heightFactor = (embers[i].y - 140.0f) / 100.0f; 
        if (heightFactor < 0.0f) heightFactor = 0.0f;

        float currentAlpha = embers[i].alpha * heightFactor;
        
        if (currentAlpha > 0.0f && embers[i].y < 240.0f && embers[i].x >= 0 && embers[i].x <= 400) {
            u8 a = (u8)(currentAlpha * 255);
            u32 color = C2D_Color32(r, g, b, a);
            C2D_DrawCircleSolid(embers[i].x, embers[i].y, 0.0f, embers[i].size, color);
        }

        if (embers[i].y <= 140.0f || currentAlpha <= 0.0f || embers[i].x < -10 || embers[i].x > 410) {
            initSingleEmber(i, false);
        }
    }
}

/* -------------------- COMET SYSTEM -------------------- */
static void spawnHourlyComet() {
    hourlyComet.startX    = -50.0f;
    hourlyComet.startY    = 120.0f;
    hourlyComet.endX      = 450.0f;
    hourlyComet.endY      = 120.0f;
    hourlyComet.progress  = 0.0f;
    hourlyComet.speed     = 0.004f;
    hourlyComet.arcHeight = -50.0f;
    hourlyComet.active    = 1;
    hourlyComet.head       = 0;
    hourlyComet.count      = 0;
    hourlyComet.trailTimer = 0;
}

static void updateHourlyComet() {
    if (!hourlyComet.active) return;

    if (hourlyComet.progress <= 1.0f)
        hourlyComet.progress += hourlyComet.speed;

    bool trailVisible = false;
    
    int firstIdx = (hourlyComet.head - hourlyComet.count + COMET_TRAIL_MAX) % COMET_TRAIL_MAX;
    for (int i = 0; i < hourlyComet.count; i++) {
        int curr = (firstIdx + i) % COMET_TRAIL_MAX;
        
        hourlyComet.trail[curr].alpha -= 0.012f;
        if (hourlyComet.trail[curr].alpha < 0.0f) hourlyComet.trail[curr].alpha = 0.0f;
        else trailVisible = true;
    }

    if (hourlyComet.progress <= 1.0f) {
        float p  = hourlyComet.progress;
        float cx = hourlyComet.startX + p * (hourlyComet.endX - hourlyComet.startX);
        float cy = (hourlyComet.startY + p * (hourlyComet.endY - hourlyComet.startY))
                 + (4.0f * hourlyComet.arcHeight * p * (1.0f - p));

        hourlyComet.trailTimer++;
        if (hourlyComet.trailTimer >= 2) {
            hourlyComet.trailTimer = 0;
            
            hourlyComet.trail[hourlyComet.head] = (TrailPoint){cx, cy, 0.8f};
            hourlyComet.head = (hourlyComet.head + 1) % COMET_TRAIL_MAX;
            if (hourlyComet.count < COMET_TRAIL_MAX) hourlyComet.count++;
        }
    }

    if (hourlyComet.progress > 1.0f && !trailVisible) {
        hourlyComet.active = 0;
        hourlyComet.count  = 0;
    }
}

static void drawHourlyComet() {
    if (!hourlyComet.active) return;

    float p  = hourlyComet.progress;
    float cx = hourlyComet.startX + p * (hourlyComet.endX - hourlyComet.startX);
    float cy = (hourlyComet.startY + p * (hourlyComet.endY - hourlyComet.startY))
                 + (4.0f * hourlyComet.arcHeight * p * (1.0f - p));

    u32 cometColor = C2D_Color32(235, 230, 180, 255);

    if (hourlyComet.count > 1) {
        int firstIdx = (hourlyComet.head - hourlyComet.count + COMET_TRAIL_MAX) % COMET_TRAIL_MAX;
        for (int i = 1; i < hourlyComet.count; i++) {
            int prev = (firstIdx + i - 1) % COMET_TRAIL_MAX;
            int curr = (firstIdx + i) % COMET_TRAIL_MAX;
            
            if (hourlyComet.trail[curr].alpha <= 0.0f) continue;
            u8 a = (u8)(hourlyComet.trail[curr].alpha * 255);
            u32 tc = C2D_Color32(230, 210, 140, a);
            C2D_DrawLine(hourlyComet.trail[prev].x, hourlyComet.trail[prev].y, tc,
                         hourlyComet.trail[curr].x, hourlyComet.trail[curr].y, tc, 4.5f, 0.0f);
        }
    }
    if (hourlyComet.count > 0 && p <= 1.0f) {
        int lastIdx = (hourlyComet.head - 1 + COMET_TRAIL_MAX) % COMET_TRAIL_MAX;
        u8 a = (u8)(hourlyComet.trail[lastIdx].alpha * 255);
        u32 tc = C2D_Color32(230, 210, 140, a);
        C2D_DrawLine(hourlyComet.trail[lastIdx].x, hourlyComet.trail[lastIdx].y, tc,
                     cx, cy, cometColor, 5.0f, 0.0f);
    }

    if (p <= 1.0f) {
        float dirX = 1.0f, dirY = 0.0f;
        if (hourlyComet.count > 0) {
            int lastIdx = (hourlyComet.head - 1 + COMET_TRAIL_MAX) % COMET_TRAIL_MAX;
            float dx = cx - hourlyComet.trail[lastIdx].x;
            float dy = cy - hourlyComet.trail[lastIdx].y;
            float len = sqrtf(dx*dx + dy*dy);
            if (len > 0.1f) { dirX = dx/len; dirY = dy/len; }
        }
        for (float i = 0.0f; i < 15.0f; i += 3.0f) {
            float s = fmaxf(1.0f, 7.5f - i*0.35f);
            C2D_DrawCircleSolid(cx - dirX*i, cy - dirY*i, 0.0f, s, C2D_Color32(235, 220, 140, 50));
        }
        for (float i = 0.0f; i < 12.0f; i += 2.0f) {
            float s = fmaxf(1.0f, 4.8f - i*0.3f);
            C2D_DrawCircleSolid(cx - dirX*i, cy - dirY*i, 0.0f, s, cometColor);
        }
        for (float i = 0.0f; i < 6.0f; i += 1.5f) {
            float s = fmaxf(0.5f, 2.5f - i*0.3f);
            C2D_DrawCircleSolid(cx - dirX*i, cy - dirY*i, 0.0f, s, C2D_Color32(255, 255, 240, 255));
        }
    }
}

/* -------------------- AIRPLANES -------------------- */
static void spawnPlane(Airplane* p) {
    if (p->active) return;
    p->y          = 15 + rand() % 50;
    p->head       = 0;
    p->count      = 0;
    p->trailTimer = 0;
    p->active     = 1; 
    if (p->direction > 0.0f) {
        p->x     = -60.0f;
        p->speed =  (0.3f + (rand() % 100) / 500.0f) * 0.85f;
    } else {
        p->x     = 460.0f;
        p->speed =  0.2f + (rand() % 100) / 600.0f;
    }
}

static void updatePlane(Airplane* p) {
    if (!p->active) return;

    if (p->active == 1) {
        p->x += p->speed * p->direction;

        p->trailTimer++;
        if (p->trailTimer >= 12) {
            p->trailTimer = 0;
            
            float tx = (p->direction > 0.0f) ? p->x        : p->x + 2.5f;
            float ty = (p->direction > 0.0f) ? p->y + 1.0f : p->y + 0.8f;
            
            p->trail[p->head] = (TrailPoint){tx, ty, (p->direction > 0.0f) ? 0.6f : 0.5f};
            p->head = (p->head + 1) % PLANE_TRAIL_MAX;
            if (p->count < PLANE_TRAIL_MAX) p->count++;
        }

        bool offscreen = (p->direction > 0.0f) ? (p->x > 460.0f) : (p->x < -60.0f);
        if (offscreen) {
            p->active = 2; 
            records.planes++;
        }
    }

    bool trailVisible = false;
    
    int firstIdx = (p->head - p->count + PLANE_TRAIL_MAX) % PLANE_TRAIL_MAX;
    for (int i = 0; i < p->count; i++) {
        int curr = (firstIdx + i) % PLANE_TRAIL_MAX;
        
        if (p->trail[curr].alpha > 0.0f) {
            p->trail[curr].alpha -= 0.002f;
            if (p->trail[curr].alpha < 0.0f) p->trail[curr].alpha = 0.0f;
            else trailVisible = true;
        }
    }

    if (p->active == 2 && !trailVisible) {
        p->active = 0;
        p->count  = 0;
    }
}

static void drawPlane(Airplane* p, float t) {
    if (!p->active) return;

    bool ltr = (p->direction > 0.0f);

    if (p->count > 1) {
        int firstIdx = (p->head - p->count + PLANE_TRAIL_MAX) % PLANE_TRAIL_MAX;
        for (int i = 1; i < p->count; i++) {
            int prev = (firstIdx + i - 1) % PLANE_TRAIL_MAX;
            int curr = (firstIdx + i) % PLANE_TRAIL_MAX;
            
            if (p->trail[curr].alpha <= 0.0f) continue;
            u8 a = (u8)(p->trail[curr].alpha * 255);
            u32 tc = C2D_Color32(200, 200, 210, a);
            float lw = ltr ? 1.2f : 1.0f;
            C2D_DrawLine(p->trail[prev].x, p->trail[prev].y, tc,
                         p->trail[curr].x, p->trail[curr].y, tc, lw, 0.0f);
        }
    }
    
    if (p->count > 0 && p->active == 1) {
        int lastIdx = (p->head - 1 + PLANE_TRAIL_MAX) % PLANE_TRAIL_MAX;
        u8  a  = (u8)(p->trail[lastIdx].alpha * 255);
        u32 tc = C2D_Color32(200, 200, 210, a);
        float ex = ltr ? p->x        : p->x + 2.5f;
        float ey = ltr ? p->y + 1.0f : p->y + 0.8f;
        float lw = ltr ? 1.2f : 1.0f;
        C2D_DrawLine(p->trail[lastIdx].x, p->trail[lastIdx].y, tc, ex, ey, tc, lw, 0.0f);
    }

    if (p->active == 1) {
        if (ltr) {
            C2D_DrawRectSolid(p->x,        p->y,        0.0f, 3.5f, 2.5f, C2D_Color32(40, 40, 50, 230));
            C2D_DrawRectSolid(p->x + 2.0f, p->y + 1.0f, 0.0f, 1.0f, 1.0f, C2D_Color32(0, 230, 50, 255));
            if (((int)(t * 60.0f)) % 40 < 4)
                C2D_DrawCircleSolid(p->x, p->y + 1.0f, 0.0f, 1.5f, C2D_Color32(255, 255, 255, 255));
        } else {
            C2D_DrawRectSolid(p->x,        p->y,        0.0f, 2.5f, 1.8f, C2D_Color32(40, 40, 50, 220));
            C2D_DrawRectSolid(p->x,        p->y + 0.5f, 0.0f, 0.8f, 0.8f, C2D_Color32(0, 230, 50, 255));
            if (((int)(t * 60.0f)) % 45 < 4)
                C2D_DrawCircleSolid(p->x + 2.5f, p->y + 0.8f, 0.0f, 1.2f, C2D_Color32(255, 255, 255, 255));
        }
    }
}

/* -------------------- CLOUDS -------------------- */
static void drawClouds(float t) {
    for (int i = 0; i < CLOUD_COUNT; i++) {
        float x  = clouds[i].x;
        float y  = clouds[i].y;
        float sc = clouds[i].scale;
        float fx = x + sinf(t * 0.05f + clouds[i].wobble) * 2.5f;
        float fy = y + cosf(t * 0.04f + clouds[i].wobble) * 1.5f;

        u8    alpha   = (u8)(170 + clouds[i].layer * 70);
        u32 colMain   = C2D_Color32(225, 230, 250, alpha);
        u32 colShadow = C2D_Color32(170, 180, 215, alpha);

        C2D_DrawCircleSolid(fx,           fy + 4*sc, 0, 18*sc, colShadow);
        C2D_DrawCircleSolid(fx - 22*sc,   fy + 6*sc, 0, 13*sc, colShadow);
        C2D_DrawCircleSolid(fx + 24*sc,   fy + 5*sc, 0, 14*sc, colShadow);
        C2D_DrawCircleSolid(fx + 44*sc,   fy + 7*sc, 0, 10*sc, colShadow);
        C2D_DrawCircleSolid(fx,           fy - 6*sc, 0, 19*sc, colMain);
        C2D_DrawCircleSolid(fx + 15*sc,   fy - 4*sc, 0, 17*sc, colMain);
        C2D_DrawCircleSolid(fx - 18*sc,   fy,        0, 14*sc, colMain);
        C2D_DrawCircleSolid(fx - 34*sc,   fy + 2*sc, 0, 10*sc, colMain);
        C2D_DrawCircleSolid(fx + 32*sc,   fy + 1*sc, 0, 13*sc, colMain);
        C2D_DrawCircleSolid(fx + 48*sc,   fy + 3*sc, 0,  9*sc, colMain);
        C2D_DrawCircleSolid(fx + 60*sc,   fy + 5*sc, 0,  6*sc, colMain);
        C2D_DrawCircleSolid(fx,           fy,        0, 16*sc, colMain);

        float dir = (i % 2 == 0) ? 1.0f : -1.0f;
        clouds[i].x += clouds[i].speed * 0.5f * dir;

        if (clouds[i].x >  500) { clouds[i].x = -120; clouds[i].y = 20 + rand() % 80; records.clouds++; }
        if (clouds[i].x < -120) { clouds[i].x =  500; clouds[i].y = 20 + rand() % 80; records.clouds++; }
    }
}

/* -------------------- STARS & SHOOTING STARS -------------------- */
static void drawStars(float t) {
    for (int i = 0; i < STAR_COUNT; i++) {
        float wave      = sinf(t * stars[i].pulseFreq + stars[i].phase);
        float blink     = 0.5f + 0.5f * wave; blink *= blink;
        float intensity = stars[i].base * (0.3f + blink * stars[i].pulseAmp);
        u8    c         = (u8)(10 + intensity * 245);
        float s         = stars[i].size * (0.6f + blink);
        C2D_DrawRectSolid(stars[i].x, stars[i].y, 0.0f, s, s, C2D_Color32(c, c, c, 255));
    }
}

static void initSingleShoot(int i) {
    int side = rand() % 3;
    if (side == 0) {
        shoots[i].x = rand() % 400; shoots[i].y = 0;
        shoots[i].vx = -2.0f + (rand() % 40) / 10.0f;
        shoots[i].vy = 2.0f  + (rand() % 30) / 10.0f;
    } else if (side == 1) {
        shoots[i].x = 0;   shoots[i].y = rand() % 120;
        shoots[i].vx = 2.0f; shoots[i].vy = 2.0f;
    } else {
        shoots[i].x = 400; shoots[i].y = rand() % 120;
        shoots[i].vx = -2.0f; shoots[i].vy = 2.0f;
    }
    shoots[i].life    = 1.0f;
    shoots[i].maxLife = 1.0f;
    shoots[i].active  = 1;
}

static void spawnShoot() {
    for (int i = 0; i < SHOOT_MAX; i++) {
        if (!shoots[i].active) {
            initSingleShoot(i);
            break;
        }
    }
}

static void updateShoot() {
    for (int i = 0; i < SHOOT_MAX; i++) {
        if (!shoots[i].active) continue;
        shoots[i].x    += shoots[i].vx;
        shoots[i].y    += shoots[i].vy;
        shoots[i].life -= 0.015f; 
        if (shoots[i].life <= 0 || shoots[i].x < -50 ||
            shoots[i].x > 450  || shoots[i].y > 260) {
            shoots[i].active = 0;
            records.shootingStars++;
        }
    }
}

static void drawShoots() {
    for (int i = 0; i < SHOOT_MAX; i++) {
        if (!shoots[i].active) continue;
        
        float a = shoots[i].life / shoots[i].maxLife;
        if (a < 0.0f) a = 0.0f;
        
        u8 alphaHead = (u8)(255 * a);
        u32 colorHead = C2D_Color32(255, 255, 255, alphaHead);
        u32 colorTail = C2D_Color32(255, 255, 255, 0); 

        C2D_DrawLine(shoots[i].x, shoots[i].y, colorHead,
                     shoots[i].x - shoots[i].vx * 6,
                     shoots[i].y - shoots[i].vy * 6,
                     colorTail, 1.5f, 0.0f);
    }
}

/* -------------------- MOON & BACKGROUND -------------------- */
static void drawMoon(float t) {
    float pulse = 0.6f + 0.4f * sinf(t * 0.2f);
    u8    core  = (u8)(190 + pulse * 30);

    float mx = 300.0f; 
    float my = 60.0f;  
    float baseRadius = 13.0f; 

    u32 colorMoon = C2D_Color32(core, core, core, 255);
    u32 colorFace = C2D_Color32(130, 130, 150, 130); 

    float glowExpansion = pulse * baseRadius * 0.5f; 

    C2D_DrawCircleSolid(mx, my, 0.0f, baseRadius * 3.5f + glowExpansion, C2D_Color32(200, 200, 230, 8));
    C2D_DrawCircleSolid(mx, my, 0.0f, baseRadius * 2.2f + glowExpansion, C2D_Color32(200, 200, 230, 18));
    C2D_DrawCircleSolid(mx, my, 0.0f, baseRadius * 1.4f + (glowExpansion * 0.5f), C2D_Color32(220, 220, 240, 35));

    C2D_DrawCircleSolid(mx, my, 0.0f, baseRadius, colorMoon);

    float eyeOffset = baseRadius * 0.45f;
    float eyeHeight = baseRadius * 0.10f;
    float eyeRadius = baseRadius * 0.28f;

    C2D_DrawCircleSolid(mx - eyeOffset, my - eyeHeight, 0.0f, eyeRadius, colorFace);
    C2D_DrawCircleSolid(mx - eyeOffset, my - (eyeHeight * 2.2f), 0.0f, eyeRadius, colorMoon);

    C2D_DrawCircleSolid(mx + eyeOffset, my - eyeHeight, 0.0f, eyeRadius, colorFace);
    C2D_DrawCircleSolid(mx + eyeOffset, my - (eyeHeight * 2.2f), 0.0f, eyeRadius, colorMoon);

    float mouthOffset = baseRadius * 0.40f;
    float mouthRadius = baseRadius * 0.22f;
    C2D_DrawCircleSolid(mx, my + mouthOffset, 0.0f, mouthRadius, colorFace);
}

static void drawBackground() {
    u32 activeTheme = (settings.bgThemeIndex < 5) ? settings.bgThemeIndex : 0;
    
    float targetTopR = skyThemes[activeTheme].topR;
    float targetTopG = skyThemes[activeTheme].topG;
    float targetTopB = skyThemes[activeTheme].topB;
    float targetBotR = skyThemes[activeTheme].botR;
    float targetBotG = skyThemes[activeTheme].botG;
    float targetBotB = skyThemes[activeTheme].botB;

    currentTopR += (targetTopR - currentTopR) * THEME_TRANSITION_SPEED;
    currentTopG += (targetTopG - currentTopG) * THEME_TRANSITION_SPEED;
    currentTopB += (targetTopB - currentTopB) * THEME_TRANSITION_SPEED;

    currentBotR += (targetBotR - currentBotR) * THEME_TRANSITION_SPEED;
    currentBotG += (targetBotG - currentBotG) * THEME_TRANSITION_SPEED;
    currentBotB += (targetBotB - currentBotB) * THEME_TRANSITION_SPEED;

    u32 topColor = C2D_Color32((u8)currentTopR, (u8)currentTopG, (u8)currentTopB, 255);
    u32 botColor = C2D_Color32((u8)currentBotR, (u8)currentBotG, (u8)currentBotB, 255);

    C2D_DrawRectangle(0, 0, 0.0f, 400, 240, topColor, topColor, botColor, botColor);
}

/* -------------------- CLOCK -------------------- */
static void drawClock(C2D_TextBuf buf, struct tm* tmv) {
    char timeStr[32], dateStr[32];

    if (settings.timeFormat24h) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
                 tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    } else {
        int hour12 = tmv->tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d %s",
                 hour12, tmv->tm_min, tmv->tm_sec,
                 (tmv->tm_hour >= 12) ? "pm" : "am");
    }

    if (settings.dateFormat == 0) // US date format
        snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
                 tmv->tm_mon+1, tmv->tm_mday, tmv->tm_year+1900);
    else if (settings.dateFormat == 1) // EU date format
        snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
                 tmv->tm_mday, tmv->tm_mon+1, tmv->tm_year+1900);
    else // ISO date format
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
                 tmv->tm_year+1900, tmv->tm_mon+1, tmv->tm_mday);

    C2D_Text timeText, dateText;
    C2D_TextParse(&timeText, buf, timeStr); C2D_TextOptimize(&timeText);
    C2D_TextParse(&dateText, buf, dateStr); C2D_TextOptimize(&dateText);

    float tScaleX = 1.35f, tScaleY = 1.60f;
    float dScaleX = 0.75f, dScaleY = 0.85f;

    if (settings.clockSizePreset == 1) {
        tScaleX *= 1.15f; tScaleY *= 1.15f;
        dScaleX *= 1.15f; dScaleY *= 1.15f;
    } else if (settings.clockSizePreset == 2) {
        tScaleX *= 1.30f; tScaleY *= 1.30f;
        dScaleX *= 1.30f; dScaleY *= 1.30f;
    }

    float tw, th, dw, dh;
    C2D_TextGetDimensions(&timeText, tScaleX, tScaleY, &tw, &th);
    C2D_TextGetDimensions(&dateText, dScaleX, dScaleY, &dw, &dh);

    const float cx = 200.0f;
    float timeX = cx - tw * 0.5f;
    float dateX = cx - dw * 0.5f;
    
    float timeY = 148.0f - th;
    float dateY = 140.0f;

    float targetR, targetG, targetB;

    if (settings.clockColorIndex == 10) {
        extern float globalAnimTime; 
        float timeFactor = globalAnimTime * 0.25f; 
        
        targetR = (sinf(timeFactor) * 50.0f) + 205.0f;
        targetG = (sinf(timeFactor + 2.09439f) * 50.0f) + 205.0f; 
        targetB = (sinf(timeFactor + 4.18879f) * 50.0f) + 205.0f; 
    } else {
        u32 targetCol = clockPresets[settings.clockColorIndex];
        targetR = (float)(targetCol & 0xFF);
        targetG = (float)((targetCol >> 8) & 0xFF);
        targetB = (float)((targetCol >> 16) & 0xFF);
    }

    currentClockR += (targetR - currentClockR) * COLOR_TRANSITION_SPEED;
    currentClockG += (targetG - currentClockG) * COLOR_TRANSITION_SPEED;
    currentClockB += (targetB - currentClockB) * COLOR_TRANSITION_SPEED;

    u32 col    = C2D_Color32((u8)currentClockR, (u8)currentClockG, (u8)currentClockB, 255);
    u32 shadow = C2D_Color32(12, 12, 24, 220);

    const float off = 2.0f;
    C2D_DrawText(&timeText, C2D_WithColor, timeX-1+off, timeY+off,   0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timeText, C2D_WithColor, timeX+1+off, timeY+off,   0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timeText, C2D_WithColor, timeX+off,   timeY-1+off, 0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timeText, C2D_WithColor, timeX+off,   timeY+1+off, 0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timeText, C2D_WithColor, timeX+off,   timeY+off,   0.48f, tScaleX, tScaleY, shadow);

    C2D_DrawText(&timeText, C2D_WithColor, timeX-1, timeY,   0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timeText, C2D_WithColor, timeX+1, timeY,   0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timeText, C2D_WithColor, timeX,   timeY-1, 0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timeText, C2D_WithColor, timeX,   timeY+1, 0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timeText, C2D_WithColor, timeX,   timeY,   0.50f, tScaleX, tScaleY, col);

    if (settings.clockMode == 0) {
        const float doff = 1.5f;
        C2D_DrawText(&dateText, C2D_WithColor, dateX-0.8f+doff, dateY+doff, 0.48f, dScaleX, dScaleY, shadow);
        C2D_DrawText(&dateText, C2D_WithColor, dateX+0.8f+doff, dateY+doff, 0.48f, dScaleX, dScaleY, shadow);
        C2D_DrawText(&dateText, C2D_WithColor, dateX+doff,      dateY+doff, 0.48f, dScaleX, dScaleY, shadow);
        C2D_DrawText(&dateText, C2D_WithColor, dateX-0.8f, dateY, 0.50f, dScaleX, dScaleY, col);
        C2D_DrawText(&dateText, C2D_WithColor, dateX+0.8f, dateY, 0.50f, dScaleX, dScaleY, col);
        C2D_DrawText(&dateText, C2D_WithColor, dateX,      dateY, 0.50f, dScaleX, dScaleY, col);
    }
}

/* -------------------- BOTTOM SCREEN MENUS -------------------- */
float globalAnimTime = 0.0f;

static void drawBottomScreen(C2D_TextBuf buf, C3D_RenderTarget* target) {
    C2D_SceneBegin(target);
    
    if (lowerScreenOff) {
        C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
        return;
    }

    u32 colYellow    = C2D_Color32(240, 200,  30, 255);
    u32 colTextBlack = C2D_Color32( 15,  15,  20, 255);
    u32 colTextWhite = C2D_Color32(255, 255, 255, 255);
    u32 colWhite     = C2D_Color32(230, 235, 245, 255);
    u32 colLightGreen= C2D_Color32( 50, 210, 100, 255);
    u32 colCyan      = C2D_Color32(  0, 191, 255, 255); 
    u32 colRed       = C2D_Color32(220,  40,  40, 255);

    if (currentScreen == SCREEN_MAIN) {
        C2D_TargetClear(target, C2D_Color32(25, 25, 35, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(60, 60, 80, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(60, 60, 80, 255));

        float tw, th;
        C2D_TextGetDimensions(&ui.title, 0.75f, 0.75f, &tw, &th);
        C2D_DrawText(&ui.title, C2D_WithColor, (320.0f-tw)*0.5f, 20, 0.5f, 0.75f, 0.75f, C2D_Color32(130,150,220,255));

        C2D_TextGetDimensions(&ui.hintSelect, 0.50f, 0.50f, &tw, &th);
        C2D_DrawText(&ui.hintSelect, C2D_WithColor, (320.0f-tw)*0.5f, 65, 0.5f, 0.50f, 0.50f, C2D_Color32(180,180,180,255));

        C2D_TextGetDimensions(&ui.hintSize, 0.50f, 0.50f, &tw, &th);
        C2D_DrawText(&ui.hintSize, C2D_WithColor, (320.0f-tw)*0.5f, 90, 0.5f, 0.50f, 0.50f, C2D_Color32(180,180,180,255));

        C2D_TextGetDimensions(&ui.hintStart, 0.50f, 0.50f, &tw, &th);
        C2D_DrawText(&ui.hintStart, C2D_WithColor, (320.0f-tw)*0.5f, 115, 0.5f, 0.50f, 0.50f, C2D_Color32(180,180,180,255));

        C2D_TextGetDimensions(&ui.hintOff, 0.50f, 0.50f, &tw, &th);
        C2D_DrawText(&ui.hintOff, C2D_WithColor, (320.0f-tw)*0.5f, 140, 0.5f, 0.50f, 0.50f, C2D_Color32(180,180,180,255));

        float bw, bh;
        C2D_DrawRectSolid(15, 185, 0.0f, 90, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnSettings, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnSettings, C2D_WithColor, 15+(90-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_DrawRectSolid(115, 185, 0.0f, 90, 32, colRed);
        C2D_TextGetDimensions(&ui.btnOff, 0.60f, 0.60f, &bw, &bh);
        C2D_DrawText(&ui.btnOff, C2D_WithColor, 115+(90-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.60f, 0.60f, colTextWhite);

        C2D_DrawRectSolid(215, 185, 0.0f, 90, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnRecords, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnRecords, C2D_WithColor, 215+(90-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
    }
    else if (currentScreen == SCREEN_RECORDS) {
        C2D_TargetClear(target, C2D_Color32(20, 20, 28, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(80, 50, 60, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(80, 50, 60, 255));

        float tw, th;
        C2D_TextGetDimensions(&ui.recTitle, 0.70f, 0.70f, &tw, &th);
        C2D_DrawText(&ui.recTitle, C2D_WithColor, (320.0f-tw)*0.5f, 25, 0.5f, 0.70f, 0.70f, C2D_Color32(230,80,100,255));

        char strStars[48], strPlanes[48], strClouds[48], strTime[64];
        snprintf(strStars,  sizeof(strStars),  "Shooting Stars : %u", (unsigned)records.shootingStars);
        snprintf(strPlanes, sizeof(strPlanes), "Planes         : %u", (unsigned)records.planes);
        snprintf(strClouds, sizeof(strClouds), "Clouds         : %u", (unsigned)records.clouds);
        u32 ts = records.totalTimeSeconds + (sessionFrames / 60);
        snprintf(strTime, sizeof(strTime), "Total Time     : %02u:%02u:%02u",
                 (unsigned)(ts/3600), (unsigned)((ts%3600)/60), (unsigned)(ts%60));

        C2D_Text tS, tP, tC, tT;
        C2D_TextParse(&tS, buf, strStars);  C2D_TextOptimize(&tS);
        C2D_TextParse(&tP, buf, strPlanes); C2D_TextOptimize(&tP);
        C2D_TextParse(&tC, buf, strClouds); C2D_TextOptimize(&tC);
        C2D_TextParse(&tT, buf, strTime);   C2D_TextOptimize(&tT);

        C2D_DrawText(&tS, C2D_WithColor, 35,  75, 0.5f, 0.55f, 0.55f, colWhite);
        C2D_DrawText(&tP, C2D_WithColor, 35, 100, 0.5f, 0.55f, 0.55f, colWhite);
        C2D_DrawText(&tC, C2D_WithColor, 35, 125, 0.5f, 0.55f, 0.55f, colWhite);
        C2D_DrawText(&tT, C2D_WithColor, 35, 155, 0.5f, 0.55f, 0.55f, C2D_Color32(140,170,220,255));

        float bw, bh;
        C2D_DrawRectSolid(15,  185, 0.0f, 110, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnResetStats, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnResetStats, C2D_WithColor, 15+(110-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_DrawRectSolid(195, 185, 0.0f, 110, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnBack, 0.6f, 0.6f, &bw, &bh);
        C2D_DrawText(&ui.btnBack, C2D_WithColor, 195+(110-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.6f, 0.6f, colTextBlack);
    }
    else if (currentScreen == SCREEN_SETTINGS) {
        C2D_TargetClear(target, C2D_Color32(30, 35, 30, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(50, 80, 50, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(50, 80, 50, 255));

        C2D_DrawText(&ui.setTitle, C2D_WithColor, 20, 15, 0.5f, 0.65f, 0.65f, colLightGreen);

        char colStr[64]; snprintf(colStr, sizeof(colStr), "Clock Color: <%s>", presetNames[settings.clockColorIndex]);
        C2D_Text tCol; C2D_TextParse(&tCol, buf, colStr); C2D_TextOptimize(&tCol);
        C2D_DrawRectSolid(15, 45, 0.0f, 290, 26, C2D_Color32(45, 55, 45, 255));
        
        u32 previewCol;
        if (settings.clockColorIndex == 10) {
            float menuFactor = globalAnimTime * 0.25f; 
            u8 pr = (u8)((sinf(menuFactor) * 50.0f) + 205.0f);
            u8 pg = (u8)((sinf(menuFactor + 2.09439f) * 50.0f) + 205.0f);
            u8 pb = (u8)((sinf(menuFactor + 4.18879f) * 50.0f) + 205.0f);
            previewCol = C2D_Color32(pr, pg, pb, 255);
        } else {
            previewCol = C2D_Color32((u8)currentClockR, (u8)currentClockG, (u8)currentClockB, 255);
        }
        C2D_DrawText(&tCol, C2D_WithColor, 25, 50, 0.5f, 0.50f, 0.50f, previewCol);

        u32 themeIndex = (settings.bgThemeIndex < 5) ? settings.bgThemeIndex : 0;
        char themeStr[64]; snprintf(themeStr, sizeof(themeStr), "Sky Theme: <%s>", skyThemes[themeIndex].name);
        C2D_Text tTheme; C2D_TextParse(&tTheme, buf, themeStr); C2D_TextOptimize(&tTheme);
        C2D_DrawRectSolid(15, 78, 0.0f, 290, 26, C2D_Color32(45, 55, 45, 255));

        u32 targetThemeCol = skyThemes[themeIndex].textMenuColor;
        float targetTR = (float)(targetThemeCol & 0xFF);
        float targetTG = (float)((targetThemeCol >> 8) & 0xFF);
        float targetTB = (float)((targetThemeCol >> 16) & 0xFF);

        currentThemeTextR += (targetTR - currentThemeTextR) * COLOR_TRANSITION_SPEED;
        currentThemeTextG += (targetTG - currentThemeTextG) * COLOR_TRANSITION_SPEED;
        currentThemeTextB += (targetTB - currentThemeTextB) * COLOR_TRANSITION_SPEED;

        u32 finalThemeTxtCol = C2D_Color32((u8)currentThemeTextR, (u8)currentThemeTextG, (u8)currentThemeTextB, 255);
        C2D_DrawText(&tTheme, C2D_WithColor, 25, 83, 0.5f, 0.50f, 0.50f, finalThemeTxtCol);

        char timeFmtStr[64]; snprintf(timeFmtStr, sizeof(timeFmtStr), "Time Format: <%s>", settings.timeFormat24h ? "24 Hours" : "12 Hours (am/pm)");
        C2D_Text tTF; C2D_TextParse(&tTF, buf, timeFmtStr); C2D_TextOptimize(&tTF);
        C2D_DrawRectSolid(15, 111, 0.0f, 290, 26, C2D_Color32(45, 55, 45, 255));
        C2D_DrawText(&tTF, C2D_WithColor, 25, 116, 0.5f, 0.50f, 0.50f, colWhite);

        char dateFmtStr[64]; snprintf(dateFmtStr, sizeof(dateFmtStr), "Date Format: <%s>", settings.dateFormat == 2 ? "YYYY-MM-DD (ISO)" : (settings.dateFormat ? "DD/MM/YYYY (EU)" : "MM/DD/YYYY (USA)"));
        C2D_Text tDF; C2D_TextParse(&tDF, buf, dateFmtStr); C2D_TextOptimize(&tDF);
        C2D_DrawRectSolid(15, 144, 0.0f, 290, 26, C2D_Color32(45, 55, 45, 255));
        C2D_DrawText(&tDF, C2D_WithColor, 25, 149, 0.5f, 0.50f, 0.50f, colWhite);

        float bw, bh;
        // Tasto Sinistro: Back
        C2D_DrawRectSolid(15,  190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnBackSet, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnBackSet, C2D_WithColor, 15+(90-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        // Tasto Centrale: Credits
        C2D_DrawRectSolid(115, 190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnCredits, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnCredits, C2D_WithColor, 115+(90-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        // Tasto Destro: Reset
        C2D_DrawRectSolid(215, 190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnResetSet, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnResetSet, C2D_WithColor, 215+(90-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
    }
    else if (currentScreen == SCREEN_CREDITS) {
        C2D_TargetClear(target, C2D_Color32(20, 25, 35, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(40, 60, 90, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(40, 60, 90, 255));

        float tw, th;
        C2D_TextGetDimensions(&ui.credTitle, 0.70f, 0.70f, &tw, &th);
        C2D_DrawText(&ui.credTitle, C2D_WithColor, (320.0f-tw)*0.5f, 25, 0.5f, 0.70f, 0.70f, colCyan);

        C2D_DrawText(&ui.credLine1, C2D_WithColor, 25, 75,  0.5f, 0.52f, 0.52f, colWhite);
        C2D_DrawText(&ui.credLine2, C2D_WithColor, 25, 100, 0.5f, 0.52f, 0.52f, colWhite);
        C2D_DrawText(&ui.credLine3, C2D_WithColor, 25, 130, 0.5f, 0.52f, 0.52f, colWhite);
        C2D_DrawText(&ui.credLine4, C2D_WithColor, 25, 155, 0.5f, 0.52f, 0.52f, colWhite);

        float bw, bh;
        C2D_DrawRectSolid(105, 185, 0.0f, 110, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnBackCred, 0.6f, 0.6f, &bw, &bh);
        C2D_DrawText(&ui.btnBackCred, C2D_WithColor, 105+(110-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.6f, 0.6f, colTextBlack);
    }
}

/* -------------------- INPUT -------------------- */
static int updateInput() {
    hidScanInput();
    u32 kDown = hidKeysDown();

    if (kDown & KEY_B) {
        if (currentScreen == SCREEN_SETTINGS) {
            if (settingsDirty) saveSettings();
            currentScreen = SCREEN_MAIN;
            return 0;
        }
        else if (currentScreen == SCREEN_RECORDS) {
            currentScreen = SCREEN_MAIN;
            return 0;
        }
        else if (currentScreen == SCREEN_CREDITS) {
            currentScreen = SCREEN_SETTINGS; 
            return 0;
        }
    }

    if (kDown & KEY_SELECT) {
        settings.clockMode = (settings.clockMode + 1) % 3;
        settingsDirty = true;
    }

    if (currentScreen == SCREEN_MAIN) {
        if (kDown & KEY_DRIGHT) {
            settings.clockSizePreset = (settings.clockSizePreset + 1) % 3;
            settingsDirty = true;
        }
        if (kDown & KEY_DLEFT) {
            settings.clockSizePreset = (settings.clockSizePreset + 2) % 3;
            settingsDirty = true;
        }
    }

    if (kDown & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);

        if (lowerScreenOff) {
            lowerScreenOff = false;
            return 0; 
        }

        if (currentScreen == SCREEN_MAIN) {
            if (touch.px >= 15 && touch.px <= 105 && touch.py >= 185 && touch.py <= 217)
                currentScreen = SCREEN_SETTINGS;
                
            if (touch.px >= 115 && touch.px <= 205 && touch.py >= 185 && touch.py <= 217) {
                lowerScreenOff = true; 
            }
                
            if (touch.px >= 215 && touch.px <= 305 && touch.py >= 185 && touch.py <= 217)
                currentScreen = SCREEN_RECORDS;
        }
        else if (currentScreen == SCREEN_SETTINGS) {
            if (touch.px >= 15 && touch.px <= 305) {
                if (touch.py >= 45 && touch.py <= 71) {
                    settings.clockColorIndex = (settings.clockColorIndex + 1) % 11; 
                    settingsDirty = true;
                }
                if (touch.py >= 78 && touch.py <= 104) {
                    settings.bgThemeIndex = (settings.bgThemeIndex + 1) % 5;
                    settingsDirty = true;
                }
                if (touch.py >= 111 && touch.py <= 137) {
                    settings.timeFormat24h = !settings.timeFormat24h;
                    settingsDirty = true;
                }
                if (touch.py >= 144 && touch.py <= 170) {
                    settings.dateFormat = (settings.dateFormat + 1) % 3;
                    settingsDirty = true;
                }
            }
            if (touch.px >= 15 && touch.px <= 105 && touch.py >= 190 && touch.py <= 220) {
                if (settingsDirty) saveSettings();
                currentScreen = SCREEN_MAIN;
            }
            if (touch.px >= 115 && touch.px <= 205 && touch.py >= 190 && touch.py <= 220) {
                currentScreen = SCREEN_CREDITS;
            }
            if (touch.px >= 215 && touch.px <= 305 && touch.py >= 190 && touch.py <= 220) {
                settings = (AppSettings){0, 1, 1, 0, 0, 0};
                saveSettings();
            }
        }
        else if (currentScreen == SCREEN_RECORDS) {
            if (touch.px >= 15 && touch.px <= 125 && touch.py >= 185 && touch.py <= 217) {
                records       = (AppRecords){0, 0, 0, 0};
                sessionFrames = 0;
                saveRecords();
            }
            if (touch.px >= 195 && touch.px <= 305 && touch.py >= 185 && touch.py <= 217)
                currentScreen = SCREEN_MAIN;
        }
        else if (currentScreen == SCREEN_CREDITS) {
            if (touch.px >= 105 && touch.px <= 215 && touch.py >= 185 && touch.py <= 217)
                currentScreen = SCREEN_SETTINGS; 
        }
    }

    if (kDown & KEY_START) return 1;
    return 0;
}

/* -------------------- MAIN -------------------- */
int main() {
    gfxInitDefault();
    C3D_Init(0x10000);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    C2D_TextBuf topBuf    = C2D_TextBufNew(1024);
    C2D_TextBuf bottomBuf = C2D_TextBufNew(2048);

    // Creiamo la directory di salvataggio una volta sola all'avvio
    mkdir(SAVE_DIR, 0777);

    loadRecords();
    loadSettings();
    initStars();
    initStaticTexts();

    int      prevHour  = -1;
    int      prevMin   = -1;
    bool     halfHourTriggered = false;

    while (aptMainLoop()) {
        if (aptShouldJumpToHome()) {
            aptJumpToHomeMenu();
        }

        if (updateInput()) break;

        sessionFrames++;

        if (sessionFrames >= 3600) {
            records.totalTimeSeconds += 60;
            sessionFrames -= 3600;
            saveRecords();
        }

        if (shootTimer > 0) shootTimer--;
        else {
            spawnShoot();
            shootTimer = 300 + (rand() % 600);
        }

        if (planeATimer > 0) planeATimer--;
        else {
            spawnPlane(&planeA);
            planeATimer = 1200 + (rand() % 9600);
        }

        if (planeBTimer > 0) planeBTimer--;
        else {
            spawnPlane(&planeB);
            planeBTimer = 1800 + (rand() % 9000);
        }

        time_t raw = time(NULL);
        struct tm* tmv = localtime(&raw);

        if (tmv->tm_hour != prevHour) {
            if (prevHour != -1) spawnHourlyComet();
            prevHour = tmv->tm_hour;
        }

        if (tmv->tm_min != prevMin) {
            halfHourTriggered = false; 
            prevMin = tmv->tm_min;
        }

        if ((tmv->tm_min == 0 || tmv->tm_min == 30) && !halfHourTriggered) {
            halfHourStarsToSpawn = 6;
            halfHourSpawnDelayTimer = 0;
            halfHourTriggered = true;
        }

        if (halfHourStarsToSpawn > 0) {
            if (halfHourSpawnDelayTimer > 0) {
                halfHourSpawnDelayTimer--;
            } else {
                for (int i = 0; i < SHOOT_MAX; i++) {
                    if (!shoots[i].active) {
                        initSingleShoot(i);
                        halfHourStarsToSpawn--;
                        halfHourSpawnDelayTimer = 18; 
                        break;
                    }
                }
            }
        }

        updateShoot();
        updatePlane(&planeA);
        updatePlane(&planeB);
        updateHourlyComet();
        globalAnimTime += 0.0166f; 

        /* ---------- TOP SCREEN ---------- */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(top);
        C2D_TextBufClear(topBuf);

        drawBackground();
        drawStars(globalAnimTime);
        drawMoon(globalAnimTime);
        drawShoots();
        drawPlane(&planeA, globalAnimTime);
        drawPlane(&planeB, globalAnimTime);
        drawHourlyComet();
        
        updateAndDrawEmbers(globalAnimTime);
        
        drawClouds(globalAnimTime);
        if (settings.clockMode != 2) drawClock(topBuf, tmv);

        /* ---------- BOTTOM SCREEN ---------- */
        C2D_TextBufClear(bottomBuf);
        drawBottomScreen(bottomBuf, bottom);

        C3D_FrameEnd(C3D_FRAME_SYNCDRAW);
    }

    if (settingsDirty) saveSettings();
    records.totalTimeSeconds += sessionFrames / 60;
    saveRecords();

    C2D_TextBufDelete(topBuf);
    C2D_TextBufDelete(bottomBuf);
    C2D_TextBufDelete(staticBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return 0;
}
