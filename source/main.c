#include <3ds.h>
#include <3ds/applets/swkbd.h>
#include <3ds/services/ptmu.h>
#include <3ds/services/mcuhwc.h>
#include <citro2d.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STAR_COUNT       80
#define SHOOT_MAX         12  
#define CLOUD_COUNT       7
#define EMBER_COUNT       30  

#define PLANE_TRAIL_MAX  32
#define COMET_TRAIL_MAX  64

#define SAVE_PATH_RECORDS  "sdmc:/3ds/NightClock/records.bin"
#define SAVE_PATH_SETTINGS "sdmc:/3ds/NightClock/settings.bin"
#define SAVE_PATH_ALARM    "sdmc:/3ds/NightClock/alarm.bin"
#define SAVE_PATH_DIARY    "sdmc:/3ds/NightClock/diary.bin"
#define SAVE_PATH_NOTES    "sdmc:/3ds/NightClock/notes.bin"
#define SAVE_DIR           "sdmc:/3ds/NightClock"

#define THEME_TRANSITION_SPEED 0.05f
#define COLOR_TRANSITION_SPEED 0.08f 

#define COMPILER_COLOR(r,g,b,a) (((u32)(a)<<24)|((u32)(b)<<16)|((u32)(g)<<8)|(u32)(r))

/* -------------------- AUDIO TEST (beep sintetico) -------------------- */
#define BEEP_SAMPLE_RATE   22050
#define BEEP_TONE_HZ       880.0f
#define BEEP_COUNT         5
#define BEEP_DURATION_MS   120
#define BEEP_GAP_MS        30
#define BEEP_VOLUME        0.75f  // 0.0 - 1.0, evita distorsione/clipping
#define ALARM_REPEAT_PAUSE_FRAMES 120  // ~2 secondi a 60fps tra un gruppo di 5 bip e il successivo
#define ALARM_FLASH_SPEED  6.0f
#define WRAP_MAX_LINES 12
#define WRAP_LINE_LEN  200

/* -------------------- STRUCTS & ENUMS -------------------- */
typedef enum {
    SCREEN_MAIN,
    SCREEN_SETTINGS,
    SCREEN_RECORDS,
    SCREEN_CREDITS,
    SCREEN_ALARM,
    SCREEN_TIMER,
    SCREEN_CALENDAR,
    SCREEN_CALENDAR_SETTINGS,
    SCREEN_CALENDAR_MONTHYEAR,
    SCREEN_DIARY_ENTRY,
    SCREEN_NOTES_LIST,
    SCREEN_NOTE_VIEW,
    SCREEN_STATS,
    SCREEN_MANUAL
} MenuScreen;

typedef enum {
    TIMER_IDLE,
    TIMER_RUNNING,
    TIMER_PAUSED
} TimerState;

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
    float clockOffsetX;
    float clockOffsetY;
    u32 calFirstDayMonday; // 0 = domenica, 1 = lunedi
} AppSettings;

typedef struct {
    u32 enabled;
    u32 hour;
    u32 minute;
} AppAlarm;

/* -------------------- CALENDAR -------------------- */
static int getDaysInMonth(int month0to11, int year) {
    static const int days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month0to11 == 1) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return leap ? 29 : 28;
    }
    return days[month0to11];
}
static int getFirstWeekdayOfMonth(int month0to11, int year) {
    struct tm firstDayTm = {0};
    firstDayTm.tm_mday = 1;
    firstDayTm.tm_mon  = month0to11;
    firstDayTm.tm_year = year - 1900;
    firstDayTm.tm_hour = 12;
    mktime(&firstDayTm);
    return firstDayTm.tm_wday;
}
static int calDisplayMonth = -1;
static int calDisplayYear  = -1;
static int calEditMonth = 0, calEditYear = 2026;

/* -------------------- DIARY -------------------- */
#define DIARY_MAX_ENTRIES 200
#define DIARY_TEXT_MAX 200
typedef struct {
    u32 used;
    u32 year, month, day;
    char text[DIARY_TEXT_MAX];
} DiaryEntry;
static DiaryEntry diaryEntries[DIARY_MAX_ENTRIES];
static int diaryViewYear = -1, diaryViewMonth = -1, diaryViewDay = -1;
#define DIARY_VISIBLE_LINES 8
static int diaryScrollOffset = 0;
static int diaryTotalLines = 0;
static float diaryScrollRepeatTimer = 0.0f;

static void loadDiary() {
    FILE* f = fopen(SAVE_PATH_DIARY, "rb");
    if (f) { fread(diaryEntries, sizeof(DiaryEntry), DIARY_MAX_ENTRIES, f); fclose(f); }
    else   { memset(diaryEntries, 0, sizeof(diaryEntries)); }
}
static void saveDiary() {
    FILE* f = fopen(SAVE_PATH_DIARY, "wb");
    if (f) { fwrite(diaryEntries, sizeof(DiaryEntry), DIARY_MAX_ENTRIES, f); fclose(f); }
}
static int findDiaryEntry(int year, int month, int day) {
    for (int i = 0; i < DIARY_MAX_ENTRIES; i++) {
        if (diaryEntries[i].used && (int)diaryEntries[i].year == year &&
            (int)diaryEntries[i].month == month && (int)diaryEntries[i].day == day)
            return i;
    }
    return -1;
}
static int getOrCreateDiaryEntry(int year, int month, int day) {
    int existing = findDiaryEntry(year, month, day);
    if (existing >= 0) return existing;
    for (int i = 0; i < DIARY_MAX_ENTRIES; i++) {
        if (!diaryEntries[i].used) {
            diaryEntries[i].used  = 1;
            diaryEntries[i].year  = year;
            diaryEntries[i].month = month;
            diaryEntries[i].day   = day;
            diaryEntries[i].text[0] = '\0';
            return i;
        }
    }
    return -1;
}
static bool findAdjacentDiaryEntry(int year, int month, int day, int direction, int* outYear, int* outMonth, int* outDay) {
    long currentKey = (long)year * 10000L + (long)month * 100L + (long)day;
    long bestKey = (direction > 0) ? 999999999L : -999999999L;
    int bestIdx = -1;
    for (int i = 0; i < DIARY_MAX_ENTRIES; i++) {
        if (!diaryEntries[i].used || !diaryEntries[i].text[0]) continue;
        long key = (long)diaryEntries[i].year * 10000L + (long)diaryEntries[i].month * 100L + (long)diaryEntries[i].day;
        if (direction > 0) {
            if (key > currentKey && key < bestKey) { bestKey = key; bestIdx = i; }
        } else {
            if (key < currentKey && key > bestKey) { bestKey = key; bestIdx = i; }
        }
    }
    if (bestIdx < 0) return false;
    *outYear  = diaryEntries[bestIdx].year;
    *outMonth = diaryEntries[bestIdx].month;
    *outDay   = diaryEntries[bestIdx].day;
    return true;
}

/* -------------------- NOTE -------------------- */
#define NOTE_MAX_ENTRIES 100
#define NOTE_TEXT_MAX 200
typedef struct {
    u32 used;
    u32 createdTime;
    u32 modifiedTime;
    char text[NOTE_TEXT_MAX];
} NoteEntry;
static NoteEntry notes[NOTE_MAX_ENTRIES];
static int noteViewIdx = -1;
#define NOTE_VISIBLE_LINES 7
static int noteScrollOffset = 0;
static int noteTotalLines = 0;
static float noteScrollRepeatTimer = 0.0f;
static bool noteDeleteConfirmPending = false;
#define NOTES_LIST_VISIBLE_COUNT 4
static int notesListScrollOffset = 0;
static int notesListSelectedIndex = 0;
static float notesListScrollRepeatTimer = 0.0f;

static void loadNotes() {
    FILE* f = fopen(SAVE_PATH_NOTES, "rb");
    if (f) { fread(notes, sizeof(NoteEntry), NOTE_MAX_ENTRIES, f); fclose(f); }
    else   { memset(notes, 0, sizeof(notes)); }
}
static void saveNotes() {
    FILE* f = fopen(SAVE_PATH_NOTES, "wb");
    if (f) { fwrite(notes, sizeof(NoteEntry), NOTE_MAX_ENTRIES, f); fclose(f); }
}
static int createNewNote() {
    time_t now = time(NULL);
    for (int i = 0; i < NOTE_MAX_ENTRIES; i++) {
        if (!notes[i].used) {
            notes[i].used = 1;
            notes[i].createdTime = (u32)now;
            notes[i].modifiedTime = (u32)now;
            notes[i].text[0] = '\0';
            return i;
        }
    }
    return -1;
}
static void deleteNote(int idx) {
    if (idx < 0 || idx >= NOTE_MAX_ENTRIES) return;
    memset(&notes[idx], 0, sizeof(NoteEntry));
    saveNotes();
}
static int getNotesOrderedByModified(int* noteOrder, int maxCount) {
    int count = 0;
    for (int i = 0; i < NOTE_MAX_ENTRIES && count < maxCount; i++) {
        if (notes[i].used) noteOrder[count++] = i;
    }
    for (int i = 1; i < count; i++) {
        int key = noteOrder[i];
        u32 keyTime = notes[key].modifiedTime;
        int j = i - 1;
        while (j >= 0 && notes[noteOrder[j]].modifiedTime < keyTime) {
            noteOrder[j + 1] = noteOrder[j]; j--;
        }
        noteOrder[j + 1] = key;
    }
    return count;
}
static void getNoteTitleLine(int idx, char* out, int outSize) {
    if (idx < 0 || !notes[idx].text[0]) { snprintf(out, outSize, "(Empty note)"); return; }
    const char* src = notes[idx].text;
    int i = 0;
    while (src[i] && src[i] != '\n' && i < outSize - 1) { out[i] = src[i]; i++; }
    out[i] = '\0';
    if (i == 0) snprintf(out, outSize, "(Empty note)");
}

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
static bool alarmDirty    = false;
static bool ringSourceIsTimer = false; // true se la suoneria è stata avviata dal timer, false se dalla sveglia

// Coordinate dei testi alarm/timer sul main screen (per renderli tappabili)
static bool  mainAlarmTextVisible = false;
static bool  mainTimerTextVisible = false;
static float mainAlarmTextX0 = 0, mainAlarmTextX1 = 0, mainAlarmTextY0 = 0, mainAlarmTextY1 = 0;
static float mainTimerTextX0 = 0, mainTimerTextX1 = 0, mainTimerTextY0 = 0, mainTimerTextY1 = 0;

// Sistema hold: tenendo premuta una freccia +/- i numeri scorrono veloci
typedef enum {
    HOLD_NONE = 0,
    HOLD_ALARM_HOUR_UP, HOLD_ALARM_HOUR_DOWN,
    HOLD_ALARM_MIN_UP,  HOLD_ALARM_MIN_DOWN,
    HOLD_TIMER_HOUR_UP, HOLD_TIMER_HOUR_DOWN,
    HOLD_TIMER_MIN_UP,  HOLD_TIMER_MIN_DOWN,
    HOLD_TIMER_SEC_UP,  HOLD_TIMER_SEC_DOWN
} HoldTarget;
static HoldTarget currentHoldTarget = HOLD_NONE;
static float holdTimer = 0.0f;
static bool  holdFirstTickDone = false;
#define HOLD_INITIAL_DELAY   0.4f
#define HOLD_REPEAT_INTERVAL 0.1f

static MenuScreen currentScreen = SCREEN_MAIN;
static AppRecords records  = {0, 0, 0, 0};
static AppSettings settings = {0, 1, 1, 0, 0, 0}; 
static AppAlarm    alarmCfg = {0, 0, 0};
static u32 sessionFrames = 0;

static int batteryPercent = 100;
static u8  batteryCharging = 0;

static bool lowerScreenOff = false;

static s16* beepBuffer = NULL;
static u32  beepBufferSamples = 0;
static ndspWaveBuf beepWaveBuf;
static bool audioReady = false;

// Editing temporaneo nella schermata Alarm (non salvato finché non si preme SET)
static u32  alarmEditHour   = 0;  // 0-23 se 24h, 1-12 se 12h (vedi settings.timeFormat24h)
static u32  alarmEditMinute = 0;
static bool alarmEditPM     = false; // usato solo quando settings.timeFormat24h è false

// Stato della sveglia mentre suona
static bool alarmRinging       = false;
static int  alarmLastTrigHour  = -1; // evita di re-triggerare più volte nello stesso minuto
static int  alarmLastTrigMin   = -1;
static u32  alarmRepeatTimer   = 0;  // countdown frame fino alla prossima ripetizione dei 5 bip
static float alarmFlashPhase   = 0.0f;

// Timer countdown: solo in RAM, niente persistenza su disco (si annulla alla chiusura dell'app)
static TimerState timerState        = TIMER_IDLE;
static u32 timerTotalSeconds        = 0;  // durata impostata (per Reset/Restart)
static u32 timerRemainingSeconds    = 0;  // secondi rimanenti, decrementati ogni secondo mentre RUNNING
static u32 timerFrameAccumulator    = 0;  // accumula i frame per scandire i secondi a 60fps

// Editing temporaneo nella schermata Timer (non salvato, è comunque un dato di sessione)
static u32 timerEditHour   = 0;
static u32 timerEditMinute = 0;
static u32 timerEditSecond = 0;

static C2D_TextBuf staticBuf;
typedef struct {
    C2D_Text title, hintMove, hintCenter, hintSelect, hintSize, hintStart, hintOff, btnSettings, btnRecords, btnOff;
    C2D_Text recTitle, btnResetStats, btnStats, btnBack;
    C2D_Text setTitle, btnBackSet, btnResetSet, btnCredits, btnAlarmSet, btnTimerSet;
    C2D_Text credTitle, credLine1, credLine2, credLine3, credLine4, btnBackCred;
    C2D_Text alarmTitle, alarmHourLabel, alarmMinLabel, btnAlarmBack, btnAlarmSetConfirm, btnAlarmClear;
    C2D_Text timerTitle, timerHourLabel, timerMinLabel, timerSecLabel, btnTimerBack;
    C2D_Text btnTimerStart, btnTimerPause, btnTimerResume, btnTimerStop, btnTimerReset;
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
    PT(&ui.title,       "NightClock");
    PT(&ui.hintMove,    "Hold L + Analog Stick to Move Clock");
    PT(&ui.hintCenter,  "Hold L + A to Center Clock");
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
    PT(&ui.btnStats,    "Manual");
    PT(&ui.btnBack,     "Back");
    
    // Settings Screen
    PT(&ui.setTitle,    "SETTINGS");
    PT(&ui.btnBackSet,  "Back");
    PT(&ui.btnResetSet, "Reset");
    PT(&ui.btnCredits,  "Credits");
    PT(&ui.btnAlarmSet, "Alarm");
    PT(&ui.btnTimerSet, "Timer");

    // Credits Screen
    PT(&ui.credTitle,   "NIGHTCLOCK CREDITS");
    PT(&ui.credLine1,   "Developed by: Michele P.");
    PT(&ui.credLine2,   "A Relaxing NightClock App");
    PT(&ui.credLine3,   "Send feedbacks to Michelep3ds@gmail.com");
    PT(&ui.credLine4,   "Version 1.04");
    PT(&ui.btnBackCred, "Back");

    // Alarm Screen
    PT(&ui.alarmTitle,        "ALARM");
    PT(&ui.alarmHourLabel,    "Hour");
    PT(&ui.alarmMinLabel,     "Minute");
    PT(&ui.btnAlarmBack,      "Back");
    PT(&ui.btnAlarmSetConfirm,"SET");
    PT(&ui.btnAlarmClear,     "Clear");

    // Timer Screen
    PT(&ui.timerTitle,      "TIMER");
    PT(&ui.timerHourLabel,  "Hour");
    PT(&ui.timerMinLabel,   "Minute");
    PT(&ui.timerSecLabel,   "Second");
    PT(&ui.btnTimerBack,    "Back");
    PT(&ui.btnTimerStart,   "Start");
    PT(&ui.btnTimerPause,   "Pause");
    PT(&ui.btnTimerResume,  "Resume");
    PT(&ui.btnTimerStop,    "Stop");
    PT(&ui.btnTimerReset,   "Reset");
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

static void saveAlarm() {
    FILE* f = fopen(SAVE_PATH_ALARM, "wb");
    if (f) { fwrite(&alarmCfg, sizeof(AppAlarm), 1, f); fclose(f); }
}

static void loadAlarm() {
    FILE* f = fopen(SAVE_PATH_ALARM, "rb");
    if (f) { fread(&alarmCfg, sizeof(AppAlarm), 1, f); fclose(f); }
    else   { alarmCfg = (AppAlarm){0, 0, 0}; }
}

/* -------------------- AUDIO -------------------- */
// Genera in RAM un buffer PCM16 mono contenente BEEP_COUNT bip ravvicinati,
// separati da brevi pause di silenzio. Tutto in un unico ndspWaveBuf.
static bool initAudio() {
    if (ndspInit() != 0) return false;

    u32 samplesPerBeep = (BEEP_SAMPLE_RATE * BEEP_DURATION_MS) / 1000;
    u32 samplesPerGap  = (BEEP_SAMPLE_RATE * BEEP_GAP_MS)      / 1000;
    beepBufferSamples  = (samplesPerBeep + samplesPerGap) * BEEP_COUNT;

    beepBuffer = (s16*)linearAlloc(beepBufferSamples * sizeof(s16));
    if (!beepBuffer) { ndspExit(); return false; }

    u32 pos = 0;
    for (int b = 0; b < BEEP_COUNT; b++) {
        // Tono: sinusoide a BEEP_TONE_HZ, con un breve fade-in/out per evitare click udibili
        for (u32 i = 0; i < samplesPerBeep; i++) {
            float t = (float)i / (float)BEEP_SAMPLE_RATE;
            float sample = sinf(2.0f * M_PI * BEEP_TONE_HZ * t);

            // Fade-in/out di 5ms su inizio e fine del singolo bip
            u32 fadeSamples = BEEP_SAMPLE_RATE * 5 / 1000;
            float env = 1.0f;
            if (i < fadeSamples) env = (float)i / (float)fadeSamples;
            else if (i > samplesPerBeep - fadeSamples) env = (float)(samplesPerBeep - i) / (float)fadeSamples;

            beepBuffer[pos++] = (s16)(sample * env * BEEP_VOLUME * 32767.0f);
        }
        // Silenzio tra un bip e il successivo (tranne dopo l'ultimo, ma non importa, è innocuo)
        for (u32 i = 0; i < samplesPerGap; i++) {
            beepBuffer[pos++] = 0;
        }
    }

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)BEEP_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);

    memset(&beepWaveBuf, 0, sizeof(beepWaveBuf));
    beepWaveBuf.data_vaddr = beepBuffer;
    beepWaveBuf.nsamples   = beepBufferSamples;
    beepWaveBuf.looping    = false;

    audioReady = true;
    return true;
}

static void exitAudio() {
    if (!audioReady) return;
    ndspChnWaveBufClear(0);
    if (beepBuffer) { linearFree(beepBuffer); beepBuffer = NULL; }
    ndspExit();
    audioReady = false;
}

// Riavvia la sequenza di 5 bip dall'inizio, anche se una riproduzione precedente è in corso
static void playTestBeep() {
    if (!audioReady) return;
    ndspChnWaveBufClear(0);
    DSP_FlushDataCache(beepBuffer, beepBufferSamples * sizeof(s16));
    beepWaveBuf.status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(0, &beepWaveBuf);
}

// Avvia la suoneria della sveglia: primo gruppo di 5 bip immediato, i successivi
// gestiti da updateAlarmRinging() con una pausa di ALARM_REPEAT_PAUSE_FRAMES tra un gruppo e l'altro.
static void startAlarmRinging(bool fromTimer) {
    alarmRinging      = true;
    ringSourceIsTimer = fromTimer;
    alarmRepeatTimer  = 0;
    alarmFlashPhase   = 0.0f;
    lowerScreenOff    = false;
}

// Ferma la suoneria e disattiva la sorgente:
// se era la sveglia, azzera alarmCfg; se era il timer, non tocca alarm.
static void stopAlarmRinging() {
    alarmRinging = false;
    if (!ringSourceIsTimer) {
        alarmCfg.enabled = 0;
        alarmDirty       = true;
    }
    ringSourceIsTimer = false;
    if (audioReady) ndspChnWaveBufClear(0);
}

// Da richiamare una volta per frame nel main loop: gestisce la ripetizione dei
// gruppi di bip con pausa, e avanza la fase del lampeggio overlay.
static void updateAlarmRinging() {
    if (!alarmRinging) return;

    if (alarmRepeatTimer == 0) {
        playTestBeep();
        // Tempo del gruppo di 5 bip (durata + gap, vedi initAudio) più la pausa tra gruppi
        u32 samplesPerBeep = (BEEP_SAMPLE_RATE * BEEP_DURATION_MS) / 1000;
        u32 samplesPerGap  = (BEEP_SAMPLE_RATE * BEEP_GAP_MS)      / 1000;
        u32 groupMs        = ((samplesPerBeep + samplesPerGap) * BEEP_COUNT * 1000) / BEEP_SAMPLE_RATE;
        u32 groupFrames    = (groupMs * 60) / 1000; // a 60fps
        alarmRepeatTimer   = groupFrames + ALARM_REPEAT_PAUSE_FRAMES;
    }
    alarmRepeatTimer--;

    alarmFlashPhase += ALARM_FLASH_SPEED * (1.0f / 60.0f);
}

// Da richiamare una volta per frame: gestisce il countdown del timer (solo se RUNNING) e,
// al raggiungimento di 0, avvia la stessa suoneria/lampeggio/popup già usati per la sveglia.
static void updateTimer() {
    if (timerState != TIMER_RUNNING) return;

    timerFrameAccumulator++;
    if (timerFrameAccumulator >= 60) { // 1 secondo reale trascorso
        timerFrameAccumulator = 0;
        if (timerRemainingSeconds > 0) {
            timerRemainingSeconds--;
        }
        if (timerRemainingSeconds == 0) {
            timerState = TIMER_IDLE;
            startAlarmRinging(true); // sorgente: timer
        }
    }
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

    float drawTopR = currentTopR, drawTopG = currentTopG, drawTopB = currentTopB;
    float drawBotR = currentBotR, drawBotG = currentBotG, drawBotB = currentBotB;

    if (alarmRinging) {
        // Mescola un rosso acceso pulsante dentro il gradiente del cielo stesso, così il
        // lampeggio tinge in modo uniforme tutta la scena (e quanto verrà disegnato sopra,
        // incluso l'orologio) senza un overlay separato con bordi visibili.
        float pulse = (sinf(alarmFlashPhase) * 0.5f) + 0.5f; // 0..1
        float flashR = 230.0f, flashG = 15.0f, flashB = 10.0f;

        drawTopR += (flashR - drawTopR) * pulse;
        drawTopG += (flashG - drawTopG) * pulse;
        drawTopB += (flashB - drawTopB) * pulse;
        drawBotR += (flashR - drawBotR) * pulse;
        drawBotG += (flashG - drawBotG) * pulse;
        drawBotB += (flashB - drawBotB) * pulse;
    }

    u32 topColor = C2D_Color32((u8)drawTopR, (u8)drawTopG, (u8)drawTopB, 255);
    u32 botColor = C2D_Color32((u8)drawBotR, (u8)drawBotG, (u8)drawBotB, 255);

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

    // Clamp X dinamico: l'orologio non può uscire dai bordi del top screen (400px),
    // calcolato sulla metà del testo più largo tra ora e data.
    float widestHalf = (tw > dw ? tw : dw) * 0.5f;
    float maxOffsetX = 198.0f - widestHalf;
    if (maxOffsetX < 0.0f) maxOffsetX = 0.0f;
    if (settings.clockOffsetX >  maxOffsetX) settings.clockOffsetX =  maxOffsetX;
    if (settings.clockOffsetX < -maxOffsetX) settings.clockOffsetX = -maxOffsetX;

    const float cx = 200.0f + settings.clockOffsetX;
    float timeX = cx - tw * 0.5f;
    float dateX = cx - dw * 0.5f;
    
    float timeY = 148.0f - th + settings.clockOffsetY;
    float dateY = 140.0f + settings.clockOffsetY;

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

    float drawClockR = currentClockR, drawClockG = currentClockG, drawClockB = currentClockB;
    if (alarmRinging) {
        float pulse = (sinf(alarmFlashPhase) * 0.5f) + 0.5f;
        drawClockR += (230.0f - drawClockR) * pulse;
        drawClockG += ( 15.0f - drawClockG) * pulse;
        drawClockB += ( 10.0f - drawClockB) * pulse;
    }

    u32 col    = C2D_Color32((u8)drawClockR, (u8)drawClockG, (u8)drawClockB, 255);
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

// Disegna il countdown del timer sul top screen, con le stesse dimensioni, colore e formato
// dell'orologio principale (vedi drawClock). Visibile solo quando il timer è avviato (RUNNING o PAUSED),
// centrato nello spazio tra la riga della data (Y~140-165) e il bordo inferiore del top screen (Y=240).
static void drawTimerCountdown(C2D_TextBuf buf) {
    if (timerState != TIMER_RUNNING && timerState != TIMER_PAUSED) return;

    u32 h = timerRemainingSeconds / 3600;
    u32 m = (timerRemainingSeconds % 3600) / 60;
    u32 s = timerRemainingSeconds % 60;
    char timerStr[16];
    snprintf(timerStr, sizeof(timerStr), "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);

    C2D_Text timerText;
    C2D_TextParse(&timerText, buf, timerStr); C2D_TextOptimize(&timerText);

    // Stesse scale usate per l'orario nell'orologio (vedi drawClock), incluso il preset di dimensione
    float tScaleX = 1.35f, tScaleY = 1.60f;
    if (settings.clockSizePreset == 1)      { tScaleX *= 1.15f; tScaleY *= 1.15f; }
    else if (settings.clockSizePreset == 2) { tScaleX *= 1.30f; tScaleY *= 1.30f; }

    float tw, th;
    C2D_TextGetDimensions(&timerText, tScaleX, tScaleY, &tw, &th);

    const float cx = 200.0f + settings.clockOffsetX;
    float timerX = cx - tw * 0.5f;
    // Centrato nello spazio libero tra la fine della riga data (~165) e il bordo inferiore (240),
    // più l'offset verticale dell'orologio per mantenersi allineato al blocco orario/data.
    float timerY = 165.0f + ((240.0f - 165.0f) - th) * 0.5f + settings.clockOffsetY;

    // Stesso colore dell'orologio (incluso il mix con il lampeggio sveglia, se attiva in contemporanea)
    float drawR = currentClockR, drawG = currentClockG, drawB = currentClockB;
    if (alarmRinging) {
        float pulse = (sinf(alarmFlashPhase) * 0.5f) + 0.5f;
        drawR += (230.0f - drawR) * pulse;
        drawG += ( 15.0f - drawG) * pulse;
        drawB += ( 10.0f - drawB) * pulse;
    }
    u32 col    = C2D_Color32((u8)drawR, (u8)drawG, (u8)drawB, 255);
    u32 shadow = C2D_Color32(12, 12, 24, 220);

    const float off = 2.0f;
    C2D_DrawText(&timerText, C2D_WithColor, timerX-1+off, timerY+off,   0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timerText, C2D_WithColor, timerX+1+off, timerY+off,   0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timerText, C2D_WithColor, timerX+off,   timerY-1+off, 0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timerText, C2D_WithColor, timerX+off,   timerY+1+off, 0.48f, tScaleX, tScaleY, shadow);
    C2D_DrawText(&timerText, C2D_WithColor, timerX+off,   timerY+off,   0.48f, tScaleX, tScaleY, shadow);

    C2D_DrawText(&timerText, C2D_WithColor, timerX-1, timerY,   0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timerText, C2D_WithColor, timerX+1, timerY,   0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timerText, C2D_WithColor, timerX,   timerY-1, 0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timerText, C2D_WithColor, timerX,   timerY+1, 0.50f, tScaleX, tScaleY, col);
    C2D_DrawText(&timerText, C2D_WithColor, timerX,   timerY,   0.50f, tScaleX, tScaleY, col);
}

/* -------------------- BOTTOM SCREEN MENUS -------------------- */
float globalAnimTime = 0.0f;

static int wrapTextIntoLines(const char* text, float maxWidth, float fontScale, C2D_TextBuf buf, char lines[][WRAP_LINE_LEN]) {
    int lineCount = 0;
    char currentLine[WRAP_LINE_LEN] = "";
    char word[WRAP_LINE_LEN];
    int wordLen = 0;
    int textLen = strlen(text);
    for (int i = 0; i <= textLen; i++) {
        char c = text[i];
        bool isBreak = (c == ' ' || c == '\n' || c == '\0');
        if (!isBreak) {
            if (wordLen < WRAP_LINE_LEN - 1) word[wordLen++] = c;
            continue;
        }
        word[wordLen] = '\0';
        if (wordLen > 0) {
            char testLine[WRAP_LINE_LEN * 2 + 2];
            if (currentLine[0]) snprintf(testLine, sizeof(testLine), "%s %s", currentLine, word);
            else                 snprintf(testLine, sizeof(testLine), "%s", word);
            C2D_Text tTest; C2D_TextParse(&tTest, buf, testLine); C2D_TextOptimize(&tTest);
            float tw, th;
            C2D_TextGetDimensions(&tTest, fontScale, fontScale, &tw, &th);
            if (tw <= maxWidth || currentLine[0] == '\0') {
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wformat-truncation"
                snprintf(currentLine, sizeof(currentLine), "%s", testLine);
                #pragma GCC diagnostic pop
            } else {
                if (lineCount < WRAP_MAX_LINES) snprintf(lines[lineCount++], WRAP_LINE_LEN, "%s", currentLine);
                snprintf(currentLine, sizeof(currentLine), "%s", word);
            }
        }
        wordLen = 0;
        if (c == '\n') {
            if (lineCount < WRAP_MAX_LINES) snprintf(lines[lineCount++], WRAP_LINE_LEN, "%s", currentLine);
            currentLine[0] = '\0';
        }
    }
    if (currentLine[0] && lineCount < WRAP_MAX_LINES)
        snprintf(lines[lineCount++], WRAP_LINE_LEN, "%s", currentLine);
    return lineCount;
}

static void drawSlimShadowTitle(const char* text, C2D_TextBuf buf, float titleX, float titleY, float titleScale, u32 titleCol, u32 titleShadow) {
    C2D_Text title;
    C2D_TextParse(&title, buf, text); C2D_TextOptimize(&title);
    const float off = 2.0f;
    C2D_DrawText(&title, C2D_WithColor, titleX+off, titleY+off, 0.48f, titleScale, titleScale, titleShadow);
    C2D_DrawText(&title, C2D_WithColor, titleX,     titleY,     0.5f,  titleScale, titleScale, titleCol);
}

static void applyHoldTargetIncrement(HoldTarget target) {
    switch (target) {
        case HOLD_ALARM_HOUR_UP:
            if (!settings.timeFormat24h) alarmEditHour = (alarmEditHour % 12) + 1;
            else                         { alarmEditHour = (alarmEditHour + 1) % 24; } break;
        case HOLD_ALARM_HOUR_DOWN:
            if (!settings.timeFormat24h) { alarmEditHour--; if (alarmEditHour < 1) alarmEditHour = 12; }
            else                         { alarmEditHour = (alarmEditHour + 23) % 24; } break;
        case HOLD_ALARM_MIN_UP:   alarmEditMinute = (alarmEditMinute + 1) % 60; break;
        case HOLD_ALARM_MIN_DOWN: alarmEditMinute = (alarmEditMinute == 0) ? 59 : alarmEditMinute - 1; break;
        case HOLD_TIMER_HOUR_UP:   timerEditHour = (timerEditHour + 1) % 24; break;
        case HOLD_TIMER_HOUR_DOWN: timerEditHour = (timerEditHour == 0) ? 23 : timerEditHour - 1; break;
        case HOLD_TIMER_MIN_UP:   timerEditMinute = (timerEditMinute + 1) % 60; break;
        case HOLD_TIMER_MIN_DOWN: timerEditMinute = (timerEditMinute == 0) ? 59 : timerEditMinute - 1; break;
        case HOLD_TIMER_SEC_UP:   timerEditSecond = (timerEditSecond + 1) % 60; break;
        case HOLD_TIMER_SEC_DOWN: timerEditSecond = (timerEditSecond == 0) ? 59 : timerEditSecond - 1; break;
        default: break;
    }
}

static HoldTarget getHoldTargetAtTouch(int px, int py) {
    if (currentScreen == SCREEN_ALARM) {
        bool is12h = !settings.timeFormat24h;
        float hourFieldX, minFieldX;
        if (is12h) {
            const float gap = 10.0f;
            const float totalW = 80.0f + gap + 80.0f + gap + 66.0f;
            hourFieldX = 15.0f + (290.0f - totalW) * 0.5f;
            minFieldX  = hourFieldX + 80.0f + gap;
        } else {
            hourFieldX = 82.0f;
            minFieldX  = 172.0f;
        }
        if (px >= hourFieldX && px <= hourFieldX + 80) {
            if (py >= 66 && py <= 82)   return HOLD_ALARM_HOUR_UP;
            if (py >= 151 && py <= 167) return HOLD_ALARM_HOUR_DOWN;
        }
        if (px >= minFieldX && px <= minFieldX + 80) {
            if (py >= 66 && py <= 82)   return HOLD_ALARM_MIN_UP;
            if (py >= 151 && py <= 167) return HOLD_ALARM_MIN_DOWN;
        }
    } else if (currentScreen == SCREEN_TIMER && timerState == TIMER_IDLE) {
        const float fieldW = 80.0f, gap = 10.0f;
        const float totalW = fieldW*3 + gap*2;
        float hourFieldX = 15.0f + (290.0f - totalW) * 0.5f;
        float minFieldX  = hourFieldX + fieldW + gap;
        float secFieldX  = minFieldX  + fieldW + gap;
        if (px >= hourFieldX && px <= hourFieldX + fieldW) {
            if (py >= 66 && py <= 82)   return HOLD_TIMER_HOUR_UP;
            if (py >= 151 && py <= 167) return HOLD_TIMER_HOUR_DOWN;
        }
        if (px >= minFieldX && px <= minFieldX + fieldW) {
            if (py >= 66 && py <= 82)   return HOLD_TIMER_MIN_UP;
            if (py >= 151 && py <= 167) return HOLD_TIMER_MIN_DOWN;
        }
        if (px >= secFieldX && px <= secFieldX + fieldW) {
            if (py >= 66 && py <= 82)   return HOLD_TIMER_SEC_UP;
            if (py >= 151 && py <= 167) return HOLD_TIMER_SEC_DOWN;
        }
    }
    return HOLD_NONE;
}

static void drawStepperButtons(float fieldX, float fieldWidth, float plusY, float minusY, float stepperH, C2D_TextBuf buf, u32 textColor) {
    float sw, sh;
    C2D_DrawRectSolid(fieldX, plusY,  0.0f, fieldWidth, stepperH, C2D_Color32(80, 65, 60, 255));
    char plusStr[2]  = "+"; C2D_Text tPlus;  C2D_TextParse(&tPlus,  buf, plusStr);  C2D_TextOptimize(&tPlus);
    C2D_TextGetDimensions(&tPlus,  0.42f, 0.42f, &sw, &sh);
    C2D_DrawText(&tPlus,  C2D_WithColor, fieldX+(fieldWidth-sw)*0.5f, plusY +(stepperH-sh)*0.5f, 0.5f, 0.42f, 0.42f, textColor);
    C2D_DrawRectSolid(fieldX, minusY, 0.0f, fieldWidth, stepperH, C2D_Color32(80, 65, 60, 255));
    char minusStr[2] = "-"; C2D_Text tMinus; C2D_TextParse(&tMinus, buf, minusStr); C2D_TextOptimize(&tMinus);
    C2D_TextGetDimensions(&tMinus, 0.42f, 0.42f, &sw, &sh);
    C2D_DrawText(&tMinus, C2D_WithColor, fieldX+(fieldWidth-sw)*0.5f, minusY+(stepperH-sh)*0.5f, 0.5f, 0.42f, 0.42f, textColor);
}

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
        const float titleScale = 0.75f * 1.4f;
        C2D_TextGetDimensions(&ui.title, titleScale, titleScale, &tw, &th);
        {
            float titleX = (320.0f - tw) * 0.5f;
            float titleY = 17.0f;
            u32 titleCol    = C2D_Color32(130, 150, 220, 255);
            u32 titleShadow = C2D_Color32(8, 8, 18, 220);
            const float off = 2.0f;
            C2D_DrawText(&ui.title, C2D_WithColor, titleX-1+off, titleY+off,   0.48f, titleScale, titleScale, titleShadow);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX+1+off, titleY+off,   0.48f, titleScale, titleScale, titleShadow);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX+off,   titleY-1+off, 0.48f, titleScale, titleScale, titleShadow);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX+off,   titleY+1+off, 0.48f, titleScale, titleScale, titleShadow);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX+off,   titleY+off,   0.48f, titleScale, titleScale, titleShadow);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX-1, titleY,   0.5f, titleScale, titleScale, titleCol);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX+1, titleY,   0.5f, titleScale, titleScale, titleCol);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX,   titleY-1, 0.5f, titleScale, titleScale, titleCol);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX,   titleY+1, 0.5f, titleScale, titleScale, titleCol);
            C2D_DrawText(&ui.title, C2D_WithColor, titleX,   titleY,   0.5f, titleScale, titleScale, titleCol);
        }

        if (alarmCfg.enabled) {
            char mainAlarmStr[48];
            if (!settings.timeFormat24h) {
                u32 h12 = alarmCfg.hour % 12; if (h12 == 0) h12 = 12;
                snprintf(mainAlarmStr, sizeof(mainAlarmStr), "Alarm set: %02u:%02u %s",
                         (unsigned)h12, (unsigned)alarmCfg.minute, (alarmCfg.hour >= 12) ? "pm" : "am");
            } else {
                snprintf(mainAlarmStr, sizeof(mainAlarmStr), "Alarm set: %02u:%02u", (unsigned)alarmCfg.hour, (unsigned)alarmCfg.minute);
            }
            C2D_Text tMainAlarm; C2D_TextParse(&tMainAlarm, buf, mainAlarmStr); C2D_TextOptimize(&tMainAlarm);
            C2D_TextGetDimensions(&tMainAlarm, 0.45f, 0.45f, &tw, &th);
            C2D_DrawText(&tMainAlarm, C2D_WithColor, (320.0f-tw)*0.5f, 45, 0.5f, 0.45f, 0.45f, C2D_Color32(240,180,100,255));
        }

        float mainAlarmBottomY = 45.0f;
        mainAlarmTextVisible = false;
        mainTimerTextVisible = false;
        if (alarmCfg.enabled) {
            char mainAlarmStr[48];
            if (!settings.timeFormat24h) {
                u32 h12 = alarmCfg.hour % 12; if (h12 == 0) h12 = 12;
                snprintf(mainAlarmStr, sizeof(mainAlarmStr), "Alarm set: %02u:%02u %s",
                         (unsigned)h12, (unsigned)alarmCfg.minute, (alarmCfg.hour >= 12) ? "pm" : "am");
            } else {
                snprintf(mainAlarmStr, sizeof(mainAlarmStr), "Alarm set: %02u:%02u", (unsigned)alarmCfg.hour, (unsigned)alarmCfg.minute);
            }
            C2D_Text tMainAlarm; C2D_TextParse(&tMainAlarm, buf, mainAlarmStr); C2D_TextOptimize(&tMainAlarm);
            C2D_TextGetDimensions(&tMainAlarm, 0.45f, 0.45f, &tw, &th);
            C2D_DrawText(&tMainAlarm, C2D_WithColor, (320.0f-tw)*0.5f, mainAlarmBottomY, 0.5f, 0.45f, 0.45f, C2D_Color32(240,180,100,255));
            mainAlarmTextVisible = true;
            mainAlarmTextX0 = (320.0f-tw)*0.5f; mainAlarmTextX1 = mainAlarmTextX0 + tw;
            mainAlarmTextY0 = mainAlarmBottomY;  mainAlarmTextY1 = mainAlarmBottomY + th;
            mainAlarmBottomY += th + 4.0f;
        }
        if (timerState == TIMER_RUNNING || timerState == TIMER_PAUSED) {
            u32 th_ = timerRemainingSeconds / 3600;
            u32 tm_ = (timerRemainingSeconds % 3600) / 60;
            u32 ts_ = timerRemainingSeconds % 60;
            char mainTimerStr[32];
            if (th_ > 0) snprintf(mainTimerStr, sizeof(mainTimerStr), "Timer: %02u:%02u:%02u", (unsigned)th_, (unsigned)tm_, (unsigned)ts_);
            else         snprintf(mainTimerStr, sizeof(mainTimerStr), "Timer: %02u:%02u", (unsigned)tm_, (unsigned)ts_);
            C2D_Text tMainTimer; C2D_TextParse(&tMainTimer, buf, mainTimerStr); C2D_TextOptimize(&tMainTimer);
            C2D_TextGetDimensions(&tMainTimer, 0.45f, 0.45f, &tw, &th);
            C2D_DrawText(&tMainTimer, C2D_WithColor, (320.0f-tw)*0.5f, mainAlarmBottomY, 0.5f, 0.45f, 0.45f, C2D_Color32(120,200,240,255));
            mainTimerTextVisible = true;
            mainTimerTextX0 = (320.0f-tw)*0.5f; mainTimerTextX1 = mainTimerTextX0 + tw;
            mainTimerTextY0 = mainAlarmBottomY;  mainTimerTextY1 = mainAlarmBottomY + th;
        }

        float bw, bh;
        // Riga superiore: Notes (sinistra), Calendar (destra)
        C2D_DrawRectSolid(15,  148, 0.0f, 90, 32, colYellow);
        char noteBtnStr[8]; snprintf(noteBtnStr, sizeof(noteBtnStr), "Notes");
        C2D_Text tNoteBtn; C2D_TextParse(&tNoteBtn, buf, noteBtnStr); C2D_TextOptimize(&tNoteBtn);
        C2D_TextGetDimensions(&tNoteBtn, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&tNoteBtn, C2D_WithColor, 15+(90-bw)*0.5f, 148+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_DrawRectSolid(215, 148, 0.0f, 90, 32, colYellow);
        char calBtnStr[16]; snprintf(calBtnStr, sizeof(calBtnStr), "Calendar");
        C2D_Text tCalBtn; C2D_TextParse(&tCalBtn, buf, calBtnStr); C2D_TextOptimize(&tCalBtn);
        C2D_TextGetDimensions(&tCalBtn, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&tCalBtn, C2D_WithColor, 215+(90-bw)*0.5f, 148+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        // Riga inferiore: Settings, Screen Off, Manual
        C2D_DrawRectSolid(15,  185, 0.0f, 90, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnSettings, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnSettings, C2D_WithColor, 15+(90-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_DrawRectSolid(115, 185, 0.0f, 90, 32, colRed);
        C2D_TextGetDimensions(&ui.btnOff, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnOff, C2D_WithColor, 115+(90-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextWhite);

        C2D_DrawRectSolid(215, 185, 0.0f, 90, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnRecords, 0.55f, 0.55f, &bw, &bh);
        C2D_DrawText(&ui.btnRecords, C2D_WithColor, 215+(90-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        // Indicatore batteria: visibile solo nel main screen, angolo in alto a destra
        {
            char battStr[16];
            snprintf(battStr, sizeof(battStr), "%d%%%s", batteryPercent, batteryCharging ? "+" : "");
            C2D_Text tBatt; C2D_TextParse(&tBatt, buf, battStr); C2D_TextOptimize(&tBatt);
            float battW, battH;
            C2D_TextGetDimensions(&tBatt, 0.45f, 0.45f, &battW, &battH);
            u32 battCol;
            if      (batteryCharging)     battCol = C2D_Color32(90, 220, 100, 255);
            else if (batteryPercent > 50) battCol = C2D_Color32(90, 220, 100, 255);
            else if (batteryPercent > 20) battCol = C2D_Color32(240, 200, 60, 255);
            else                          battCol = C2D_Color32(230, 60,  60, 255);
            C2D_DrawText(&tBatt, C2D_WithColor, 310.0f - battW, 16, 0.5f, 0.45f, 0.45f, battCol);
        }
    }
    else if (currentScreen == SCREEN_RECORDS) {
        C2D_TargetClear(target, C2D_Color32(20, 20, 28, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(80, 50, 60, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(80, 50, 60, 255));

        float tw, th;
        C2D_TextGetDimensions(&ui.recTitle, 0.70f, 0.70f, &tw, &th);
        C2D_DrawText(&ui.recTitle, C2D_WithColor, (320.0f-tw)*0.5f, 22, 0.5f, 0.70f, 0.70f, C2D_Color32(230,80,100,255));

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

        C2D_DrawText(&tS, C2D_WithColor, 35,  65, 0.5f, 0.55f, 0.55f, colWhite);
        C2D_DrawText(&tP, C2D_WithColor, 35,  90, 0.5f, 0.55f, 0.55f, colWhite);
        C2D_DrawText(&tC, C2D_WithColor, 35, 115, 0.5f, 0.55f, 0.55f, colWhite);
        C2D_DrawText(&tT, C2D_WithColor, 35, 145, 0.5f, 0.55f, 0.55f, C2D_Color32(140,170,220,255));

        // 3 bottoni: Manual, Reset Stats, Back — area 15-305 (290px), gap 5px, bottoni 93px
        const float bBtnW = 93.0f, bBtnGap = 5.0f;
        float bBtnX[3];
        bBtnX[0] = 15.0f;
        bBtnX[1] = bBtnX[0] + bBtnW + bBtnGap;
        bBtnX[2] = bBtnX[1] + bBtnW + bBtnGap;

        float bw, bh;
        C2D_DrawRectSolid(bBtnX[0], 185, 0.0f, bBtnW, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnStats, 0.48f, 0.48f, &bw, &bh);
        C2D_DrawText(&ui.btnStats, C2D_WithColor, bBtnX[0]+(bBtnW-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.48f, 0.48f, colTextBlack);

        C2D_DrawRectSolid(bBtnX[1], 185, 0.0f, bBtnW, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnResetStats, 0.48f, 0.48f, &bw, &bh);
        C2D_DrawText(&ui.btnResetStats, C2D_WithColor, bBtnX[1]+(bBtnW-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.48f, 0.48f, colTextBlack);

        C2D_DrawRectSolid(bBtnX[2], 185, 0.0f, bBtnW, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnBack, 0.48f, 0.48f, &bw, &bh);
        C2D_DrawText(&ui.btnBack, C2D_WithColor, bBtnX[2]+(bBtnW-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.48f, 0.48f, colTextBlack);
    }
    else if (currentScreen == SCREEN_MANUAL) {
        C2D_TargetClear(target, C2D_Color32(20, 20, 28, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(80, 50, 60, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(80, 50, 60, 255));
        drawSlimShadowTitle("MANUAL", buf, 20, 15, 0.65f, C2D_Color32(200,180,140,255), C2D_Color32(10,8,6,220));

        float tw, th;
        const float mY0 = 42.0f, mGap = 19.0f;
        u32 mCol = C2D_Color32(180,180,190,255);
        C2D_TextGetDimensions(&ui.hintMove,   0.44f, 0.44f, &tw, &th);
        C2D_DrawText(&ui.hintMove,   C2D_WithColor, (320.0f-tw)*0.5f, mY0,        0.5f, 0.44f, 0.44f, mCol);
        C2D_TextGetDimensions(&ui.hintCenter, 0.44f, 0.44f, &tw, &th);
        C2D_DrawText(&ui.hintCenter, C2D_WithColor, (320.0f-tw)*0.5f, mY0+mGap,   0.5f, 0.44f, 0.44f, mCol);
        C2D_TextGetDimensions(&ui.hintSelect, 0.44f, 0.44f, &tw, &th);
        C2D_DrawText(&ui.hintSelect, C2D_WithColor, (320.0f-tw)*0.5f, mY0+mGap*2, 0.5f, 0.44f, 0.44f, mCol);
        C2D_TextGetDimensions(&ui.hintSize,   0.44f, 0.44f, &tw, &th);
        C2D_DrawText(&ui.hintSize,   C2D_WithColor, (320.0f-tw)*0.5f, mY0+mGap*3, 0.5f, 0.44f, 0.44f, mCol);
        C2D_TextGetDimensions(&ui.hintStart,  0.44f, 0.44f, &tw, &th);
        C2D_DrawText(&ui.hintStart,  C2D_WithColor, (320.0f-tw)*0.5f, mY0+mGap*4, 0.5f, 0.44f, 0.44f, mCol);
        C2D_TextGetDimensions(&ui.hintOff,    0.44f, 0.44f, &tw, &th);
        C2D_DrawText(&ui.hintOff,    C2D_WithColor, (320.0f-tw)*0.5f, mY0+mGap*5, 0.5f, 0.44f, 0.44f, mCol);

        float bw, bh;
        C2D_DrawRectSolid(211, 185, 0.0f, 93, 32, colYellow);
        C2D_TextGetDimensions(&ui.btnBack, 0.48f, 0.48f, &bw, &bh);
        C2D_DrawText(&ui.btnBack, C2D_WithColor, 211+(93-bw)*0.5f, 185+(32-bh)*0.5f, 0.5f, 0.48f, 0.48f, colTextBlack);
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
        // Tasto 1: Back
        C2D_DrawRectSolid(15,  190, 0.0f, 54, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnBackSet, 0.45f, 0.45f, &bw, &bh);
        C2D_DrawText(&ui.btnBackSet, C2D_WithColor, 15+(54-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.45f, 0.45f, colTextBlack);

        // Tasto 2: Credits
        C2D_DrawRectSolid(74,  190, 0.0f, 54, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnCredits, 0.45f, 0.45f, &bw, &bh);
        C2D_DrawText(&ui.btnCredits, C2D_WithColor, 74+(54-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.45f, 0.45f, colTextBlack);

        // Tasto 3: Alarm
        C2D_DrawRectSolid(133, 190, 0.0f, 54, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnAlarmSet, 0.45f, 0.45f, &bw, &bh);
        C2D_DrawText(&ui.btnAlarmSet, C2D_WithColor, 133+(54-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.45f, 0.45f, colTextBlack);

        // Tasto 4: Timer
        C2D_DrawRectSolid(192, 190, 0.0f, 54, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnTimerSet, 0.45f, 0.45f, &bw, &bh);
        C2D_DrawText(&ui.btnTimerSet, C2D_WithColor, 192+(54-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.45f, 0.45f, colTextBlack);

        // Tasto 5: Reset
        C2D_DrawRectSolid(251, 190, 0.0f, 54, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnResetSet, 0.45f, 0.45f, &bw, &bh);
        C2D_DrawText(&ui.btnResetSet, C2D_WithColor, 251+(54-bw)*0.5f, 190+(30-bh)*0.5f, 0.5f, 0.45f, 0.45f, colTextBlack);
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
    else if (currentScreen == SCREEN_ALARM) {
        C2D_TargetClear(target, C2D_Color32(35, 25, 30, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(90, 60, 50, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(90, 60, 50, 255));
        bool is12h = !settings.timeFormat24h;
        float tw, th;
        C2D_TextGetDimensions(&ui.alarmTitle, 0.70f, 0.70f, &tw, &th);
        drawSlimShadowTitle("ALARM", buf, (320.0f-tw)*0.5f, 22, 0.70f, C2D_Color32(240,160,80,255), C2D_Color32(14, 8, 6, 220));
        char statusStr[48];
        if (alarmCfg.enabled) {
            if (is12h) {
                u32 h12 = alarmCfg.hour % 12; if (h12 == 0) h12 = 12;
                snprintf(statusStr, sizeof(statusStr), "Alarm set: %02u:%02u %s",
                         (unsigned)h12, (unsigned)alarmCfg.minute, (alarmCfg.hour >= 12) ? "pm" : "am");
            } else {
                snprintf(statusStr, sizeof(statusStr), "Alarm set: %02u:%02u", (unsigned)alarmCfg.hour, (unsigned)alarmCfg.minute);
            }
        } else {
            snprintf(statusStr, sizeof(statusStr), "Alarm is OFF");
        }
        C2D_Text tStatus; C2D_TextParse(&tStatus, buf, statusStr); C2D_TextOptimize(&tStatus);
        C2D_TextGetDimensions(&tStatus, 0.45f, 0.45f, &tw, &th);
        C2D_DrawText(&tStatus, C2D_WithColor, (320.0f-tw)*0.5f, 42, 0.5f, 0.45f, 0.45f, colWhite);
        float hourFieldX, minFieldX, ampmFieldX;
        if (is12h) {
            const float gap = 10.0f;
            const float totalW = 80.0f + gap + 80.0f + gap + 66.0f;
            const float startX = 15.0f + (290.0f - totalW) * 0.5f;
            hourFieldX = startX;
            minFieldX  = hourFieldX + 80.0f + gap;
            ampmFieldX = minFieldX  + 80.0f + gap;
        } else {
            hourFieldX = 82.0f;
            minFieldX  = 172.0f;
            ampmFieldX = 0.0f;
        }
        C2D_DrawRectSolid(hourFieldX, 80, 0.0f, 80, 70, C2D_Color32(50, 40, 40, 255));
        C2D_TextGetDimensions(&ui.alarmHourLabel, 0.45f, 0.45f, &tw, &th);
        C2D_DrawText(&ui.alarmHourLabel, C2D_WithColor, hourFieldX+(80-tw)*0.5f, 86, 0.5f, 0.45f, 0.45f, C2D_Color32(200,150,120,255));
        char hourStr[8]; snprintf(hourStr, sizeof(hourStr), "%02u", (unsigned)alarmEditHour);
        C2D_Text tHour; C2D_TextParse(&tHour, buf, hourStr); C2D_TextOptimize(&tHour);
        C2D_TextGetDimensions(&tHour, 0.9f, 0.9f, &tw, &th);
        C2D_DrawText(&tHour, C2D_WithColor, hourFieldX+(80-tw)*0.5f, 108, 0.5f, 0.9f, 0.9f, colTextWhite);
        drawStepperButtons(hourFieldX, 80.0f, 66.0f, 151.0f, 16.0f, buf, colTextWhite);
        C2D_DrawRectSolid(minFieldX, 80, 0.0f, 80, 70, C2D_Color32(50, 40, 40, 255));
        C2D_TextGetDimensions(&ui.alarmMinLabel, 0.45f, 0.45f, &tw, &th);
        C2D_DrawText(&ui.alarmMinLabel, C2D_WithColor, minFieldX+(80-tw)*0.5f, 86, 0.5f, 0.45f, 0.45f, C2D_Color32(200,150,120,255));
        char minStr[8]; snprintf(minStr, sizeof(minStr), "%02u", (unsigned)alarmEditMinute);
        C2D_Text tMin; C2D_TextParse(&tMin, buf, minStr); C2D_TextOptimize(&tMin);
        C2D_TextGetDimensions(&tMin, 0.9f, 0.9f, &tw, &th);
        C2D_DrawText(&tMin, C2D_WithColor, minFieldX+(80-tw)*0.5f, 108, 0.5f, 0.9f, 0.9f, colTextWhite);
        drawStepperButtons(minFieldX, 80.0f, 66.0f, 151.0f, 16.0f, buf, colTextWhite);
        if (is12h) {
            C2D_DrawRectSolid(ampmFieldX, 91, 0.0f, 66, 48, C2D_Color32(50, 40, 40, 255));
            char ampmStr[4]; snprintf(ampmStr, sizeof(ampmStr), "%s", alarmEditPM ? "PM" : "AM");
            C2D_Text tAmpm; C2D_TextParse(&tAmpm, buf, ampmStr); C2D_TextOptimize(&tAmpm);
            C2D_TextGetDimensions(&tAmpm, 0.72f, 0.72f, &tw, &th);
            C2D_DrawText(&tAmpm, C2D_WithColor, ampmFieldX+(66-tw)*0.5f, 91+(48-th)*0.5f, 0.5f, 0.72f, 0.72f, colTextWhite);
        }
        float abw, abh;
        C2D_DrawRectSolid(15,  190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnAlarmBack, 0.55f, 0.55f, &abw, &abh);
        C2D_DrawText(&ui.btnAlarmBack, C2D_WithColor, 15+(90-abw)*0.5f, 190+(30-abh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
        C2D_DrawRectSolid(115, 190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnAlarmSetConfirm, 0.55f, 0.55f, &abw, &abh);
        C2D_DrawText(&ui.btnAlarmSetConfirm, C2D_WithColor, 115+(90-abw)*0.5f, 190+(30-abh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
        C2D_DrawRectSolid(215, 190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnAlarmClear, 0.55f, 0.55f, &abw, &abh);
        C2D_DrawText(&ui.btnAlarmClear, C2D_WithColor, 215+(90-abw)*0.5f, 190+(30-abh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
    }
    else if (currentScreen == SCREEN_TIMER) {
        C2D_TargetClear(target, C2D_Color32(25, 30, 38, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(60, 80, 95, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(60, 80, 95, 255));
        float tw, th;
        C2D_TextGetDimensions(&ui.timerTitle, 0.70f, 0.70f, &tw, &th);
        drawSlimShadowTitle("TIMER", buf, (320.0f-tw)*0.5f, 22, 0.70f, C2D_Color32(110,180,230,255), C2D_Color32(6, 10, 14, 220));
        char statusStr[48];
        if (timerState == TIMER_RUNNING || timerState == TIMER_PAUSED) {
            u32 h = timerRemainingSeconds / 3600;
            u32 m = (timerRemainingSeconds % 3600) / 60;
            u32 s = timerRemainingSeconds % 60;
            snprintf(statusStr, sizeof(statusStr), "%s: %02u:%02u:%02u",
                     (timerState == TIMER_PAUSED) ? "Paused" : "Running", (unsigned)h, (unsigned)m, (unsigned)s);
        } else {
            snprintf(statusStr, sizeof(statusStr), "Timer not set");
        }
        C2D_Text tStatus; C2D_TextParse(&tStatus, buf, statusStr); C2D_TextOptimize(&tStatus);
        C2D_TextGetDimensions(&tStatus, 0.45f, 0.45f, &tw, &th);
        C2D_DrawText(&tStatus, C2D_WithColor, (320.0f-tw)*0.5f, 42, 0.5f, 0.45f, 0.45f, colWhite);
        const float fieldW = 80.0f, gap = 10.0f;
        const float totalW = fieldW*3 + gap*2;
        const float startX = 15.0f + (290.0f - totalW) * 0.5f;
        float hourFieldX = startX;
        float minFieldX  = hourFieldX + fieldW + gap;
        float secFieldX  = minFieldX  + fieldW + gap;
        bool fieldsEditable = (timerState == TIMER_IDLE);
        u32 fieldBgCol = fieldsEditable ? C2D_Color32(40, 48, 58, 255) : C2D_Color32(30, 34, 40, 255);
        u32 fieldTxtCol = fieldsEditable ? colTextWhite : C2D_Color32(140, 145, 150, 255);
        C2D_DrawRectSolid(hourFieldX, 80, 0.0f, fieldW, 70, fieldBgCol);
        C2D_TextGetDimensions(&ui.timerHourLabel, 0.42f, 0.42f, &tw, &th);
        C2D_DrawText(&ui.timerHourLabel, C2D_WithColor, hourFieldX+(fieldW-tw)*0.5f, 86, 0.5f, 0.42f, 0.42f, C2D_Color32(150,170,190,255));
        char hourStr[8]; snprintf(hourStr, sizeof(hourStr), "%02u", (unsigned)timerEditHour);
        C2D_Text tHour; C2D_TextParse(&tHour, buf, hourStr); C2D_TextOptimize(&tHour);
        C2D_TextGetDimensions(&tHour, 0.85f, 0.85f, &tw, &th);
        C2D_DrawText(&tHour, C2D_WithColor, hourFieldX+(fieldW-tw)*0.5f, 108, 0.5f, 0.85f, 0.85f, fieldTxtCol);
        C2D_DrawRectSolid(minFieldX, 80, 0.0f, fieldW, 70, fieldBgCol);
        C2D_TextGetDimensions(&ui.timerMinLabel, 0.42f, 0.42f, &tw, &th);
        C2D_DrawText(&ui.timerMinLabel, C2D_WithColor, minFieldX+(fieldW-tw)*0.5f, 86, 0.5f, 0.42f, 0.42f, C2D_Color32(150,170,190,255));
        char minStr[8]; snprintf(minStr, sizeof(minStr), "%02u", (unsigned)timerEditMinute);
        C2D_Text tMin; C2D_TextParse(&tMin, buf, minStr); C2D_TextOptimize(&tMin);
        C2D_TextGetDimensions(&tMin, 0.85f, 0.85f, &tw, &th);
        C2D_DrawText(&tMin, C2D_WithColor, minFieldX+(fieldW-tw)*0.5f, 108, 0.5f, 0.85f, 0.85f, fieldTxtCol);

        // Campo Secondi
        C2D_DrawRectSolid(secFieldX, 80, 0.0f, fieldW, 70, fieldBgCol);
        C2D_TextGetDimensions(&ui.timerSecLabel, 0.42f, 0.42f, &tw, &th);
        C2D_DrawText(&ui.timerSecLabel, C2D_WithColor, secFieldX+(fieldW-tw)*0.5f, 86, 0.5f, 0.42f, 0.42f, C2D_Color32(150,170,190,255));
        char secStr[8]; snprintf(secStr, sizeof(secStr), "%02u", (unsigned)timerEditSecond);
        C2D_Text tSec; C2D_TextParse(&tSec, buf, secStr); C2D_TextOptimize(&tSec);
        C2D_TextGetDimensions(&tSec, 0.85f, 0.85f, &tw, &th);
        C2D_DrawText(&tSec, C2D_WithColor, secFieldX+(fieldW-tw)*0.5f, 108, 0.5f, 0.85f, 0.85f, fieldTxtCol);

        if (fieldsEditable) {
            drawStepperButtons(hourFieldX, fieldW, 66.0f, 151.0f, 16.0f, buf, fieldTxtCol);
            drawStepperButtons(minFieldX,  fieldW, 66.0f, 151.0f, 16.0f, buf, fieldTxtCol);
            drawStepperButtons(secFieldX,  fieldW, 66.0f, 151.0f, 16.0f, buf, fieldTxtCol);
        }

        // Bottoni contestuali allo stato: sempre 3 nella stessa posizione (Back, centrale, destro)
        float tbw, tbh;
        C2D_DrawRectSolid(15,  190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(&ui.btnTimerBack, 0.55f, 0.55f, &tbw, &tbh);
        C2D_DrawText(&ui.btnTimerBack, C2D_WithColor, 15+(90-tbw)*0.5f, 190+(30-tbh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_Text* centerBtn;
        C2D_Text* rightBtn;
        if (timerState == TIMER_IDLE)         { centerBtn = &ui.btnTimerStart;  rightBtn = &ui.btnTimerReset; }
        else if (timerState == TIMER_RUNNING) { centerBtn = &ui.btnTimerPause;  rightBtn = &ui.btnTimerStop;  }
        else /* TIMER_PAUSED */               { centerBtn = &ui.btnTimerResume; rightBtn = &ui.btnTimerStop;  }

        C2D_DrawRectSolid(115, 190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(centerBtn, 0.55f, 0.55f, &tbw, &tbh);
        C2D_DrawText(centerBtn, C2D_WithColor, 115+(90-tbw)*0.5f, 190+(30-tbh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_DrawRectSolid(215, 190, 0.0f, 90, 30, colYellow);
        C2D_TextGetDimensions(rightBtn, 0.55f, 0.55f, &tbw, &tbh);
        C2D_DrawText(rightBtn, C2D_WithColor, 215+(90-tbw)*0.5f, 190+(30-tbh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
    }
    else if (currentScreen == SCREEN_CALENDAR) {
        C2D_TargetClear(target, C2D_Color32(22, 26, 34, 255));

        time_t rawCal = time(NULL);
        struct tm* tmCal = localtime(&rawCal);
        int realYear  = tmCal->tm_year + 1900;
        int realMonth = tmCal->tm_mon;
        int realToday = tmCal->tm_mday;
        if (calDisplayMonth < 0) { calDisplayMonth = realMonth; calDisplayYear = realYear; }
        int calMonth = calDisplayMonth;
        int calYear  = calDisplayYear;

        static const char* calMonthNames[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        char calHeaderStr[48];
        snprintf(calHeaderStr, sizeof(calHeaderStr), "%02d:%02d  %s %d",
                 tmCal->tm_hour, tmCal->tm_min, calMonthNames[calMonth], calYear);
        C2D_Text tCalHeader; C2D_TextParse(&tCalHeader, buf, calHeaderStr); C2D_TextOptimize(&tCalHeader);
        float chw, chh;
        C2D_TextGetDimensions(&tCalHeader, 0.52f, 0.52f, &chw, &chh);
        C2D_DrawText(&tCalHeader, C2D_WithColor, (320.0f-chw)*0.5f, 14, 0.5f, 0.52f, 0.52f, C2D_Color32(255,255,255,255));
        float headerCenterY = 14.0f + chh * 0.5f;

        char calPrevStr[2] = "<"; C2D_Text tCalPrev; C2D_TextParse(&tCalPrev, buf, calPrevStr); C2D_TextOptimize(&tCalPrev);
        float cpw, cph; C2D_TextGetDimensions(&tCalPrev, 0.8f, 0.8f, &cpw, &cph);
        C2D_DrawText(&tCalPrev, C2D_WithColor, 92.0f - cpw*0.5f, headerCenterY - cph*0.5f, 0.5f, 0.8f, 0.8f, C2D_Color32(255,255,255,255));
        char calNextStr[2] = ">"; C2D_Text tCalNext; C2D_TextParse(&tCalNext, buf, calNextStr); C2D_TextOptimize(&tCalNext);
        float cnw, cnh; C2D_TextGetDimensions(&tCalNext, 0.8f, 0.8f, &cnw, &cnh);
        C2D_DrawText(&tCalNext, C2D_WithColor, 227.0f - cnw*0.5f, headerCenterY - cnh*0.5f, 0.5f, 0.8f, 0.8f, C2D_Color32(255,255,255,255));

        C2D_DrawRectSolid(15.0f, 12.0f, 0.0f, 60.0f, 20.0f, colYellow);
        char calSettingsStr[16]; snprintf(calSettingsStr, sizeof(calSettingsStr), "Settings");
        C2D_Text tCalSettings; C2D_TextParse(&tCalSettings, buf, calSettingsStr); C2D_TextOptimize(&tCalSettings);
        float cstw, csth;
        C2D_TextGetDimensions(&tCalSettings, 0.36f, 0.36f, &cstw, &csth);
        C2D_DrawText(&tCalSettings, C2D_WithColor, 15.0f+(60.0f-cstw)*0.5f, 12.0f+(20.0f-csth)*0.5f, 0.5f, 0.36f, 0.36f, colTextBlack);

        static const char* calDayLettersSun[7] = {"S","M","T","W","T","F","S"};
        static const char* calDayLettersMon[7] = {"M","T","W","T","F","S","S"};
        const char** calDayLetters = settings.calFirstDayMonday ? calDayLettersMon : calDayLettersSun;
        const float calL = 15.0f, calR = 305.0f;
        const float colW = (calR - calL) / 7.0f;
        const float dayLabelY = 34.0f;
        for (int c = 0; c < 7; c++) {
            char dl[2]; snprintf(dl, sizeof(dl), "%s", calDayLetters[c]);
            C2D_Text tDl; C2D_TextParse(&tDl, buf, dl); C2D_TextOptimize(&tDl);
            float dlw, dlh;
            C2D_TextGetDimensions(&tDl, 0.42f, 0.42f, &dlw, &dlh);
            float cellCenterX = calL + colW * c + colW * 0.5f;
            C2D_DrawText(&tDl, C2D_WithColor, cellCenterX - dlw*0.5f, dayLabelY, 0.5f, 0.42f, 0.42f, C2D_Color32(150,180,220,255));
        }

        int daysInMonth   = getDaysInMonth(calMonth, calYear);
        int firstWeekdayRaw = getFirstWeekdayOfMonth(calMonth, calYear);
        int firstWeekday = settings.calFirstDayMonday ? (firstWeekdayRaw + 6) % 7 : firstWeekdayRaw;
        const float gridTopY = 54.0f, gridBottomY = 222.0f;
        const float rowH = (gridBottomY - gridTopY) / 6.0f;

        u32 gridLineColor = C2D_Color32(60, 70, 90, 255);
        for (int c2 = 0; c2 <= 7; c2++) {
            float lineX = calL + colW * c2;
            C2D_DrawRectSolid(lineX, gridTopY, 0.0f, 1.0f, gridBottomY - gridTopY, gridLineColor);
        }
        for (int r = 0; r <= 6; r++) {
            float lineY = gridTopY + rowH * r;
            C2D_DrawRectSolid(calL, lineY, 0.0f, calR - calL, 1.0f, gridLineColor);
        }

        bool isCurrentMonthShown = (calMonth == realMonth && calYear == realYear);
        for (int row = 0; row < 6; row++) {
            for (int col = 0; col < 7; col++) {
                int dayNum = row * 7 + col - firstWeekday + 1;
                if (dayNum < 1 || dayNum > daysInMonth) continue;
                float cellX = calL + colW * col;
                float cellY = gridTopY + rowH * row;
                bool isToday = isCurrentMonthShown && (dayNum == realToday);
                if (isToday) {
                    C2D_DrawRectSolid(cellX + 3, cellY + 2, 0.0f, colW - 6, rowH - 4, C2D_Color32(90, 130, 190, 255));
                }
                char dayStr[12]; snprintf(dayStr, sizeof(dayStr), "%d", dayNum);
                C2D_Text tDay; C2D_TextParse(&tDay, buf, dayStr); C2D_TextOptimize(&tDay);
                float dw2, dh2;
                C2D_TextGetDimensions(&tDay, 0.40f, 0.40f, &dw2, &dh2);
                float cellCenterX = cellX + colW * 0.5f;
                float cellCenterY = cellY + rowH * 0.5f;
                u32 dayColor = isToday ? C2D_Color32(255,255,255,255) : C2D_Color32(200,205,215,255);
                C2D_DrawText(&tDay, C2D_WithColor, cellCenterX - dw2*0.5f, cellCenterY - dh2*0.5f, 0.5f, 0.40f, 0.40f, dayColor);
                if (findDiaryEntry(calYear, calMonth, dayNum) >= 0) {
                    C2D_DrawRectSolid(cellCenterX - 2.0f, cellY + rowH - 8.0f, 0.0f, 4.0f, 4.0f, C2D_Color32(230, 190, 90, 255));
                }
            }
        }

        C2D_DrawRectSolid(245.0f, 12.0f, 0.0f, 60.0f, 20.0f, colYellow);
        char calBackStr[8]; snprintf(calBackStr, sizeof(calBackStr), "Back");
        C2D_Text tCalBack; C2D_TextParse(&tCalBack, buf, calBackStr); C2D_TextOptimize(&tCalBack);
        float cbw, cbh;
        C2D_TextGetDimensions(&tCalBack, 0.40f, 0.40f, &cbw, &cbh);
        C2D_DrawText(&tCalBack, C2D_WithColor, 245.0f+(60.0f-cbw)*0.5f, 12.0f+(20.0f-cbh)*0.5f, 0.5f, 0.40f, 0.40f, colTextBlack);
    }
    else if (currentScreen == SCREEN_CALENDAR_SETTINGS) {
        C2D_TargetClear(target, C2D_Color32(22, 26, 34, 255));
        C2D_DrawRectSolid(10, 10,  0.0f, 300, 2, C2D_Color32(90, 110, 150, 255));
        C2D_DrawRectSolid(10, 228, 0.0f, 300, 2, C2D_Color32(90, 110, 150, 255));
        drawSlimShadowTitle("CALENDAR SETTINGS", buf, 20, 15, 0.6f, C2D_Color32(150,180,220,255), C2D_Color32(8, 10, 16, 220));

        C2D_DrawRectSolid(15, 55, 0.0f, 290, 32, C2D_Color32(45, 55, 65, 255));
        char fdwLabelStr[24]; snprintf(fdwLabelStr, sizeof(fdwLabelStr), "First Day of Week:");
        C2D_Text tFdwLabel; C2D_TextParse(&tFdwLabel, buf, fdwLabelStr); C2D_TextOptimize(&tFdwLabel);
        float fdwlw, fdwlh;
        C2D_TextGetDimensions(&tFdwLabel, 0.44f, 0.44f, &fdwlw, &fdwlh);
        C2D_DrawText(&tFdwLabel, C2D_WithColor, 25, 55+(32-fdwlh)*0.5f, 0.5f, 0.44f, 0.44f, colWhite);
        char fdwValueStr[16]; snprintf(fdwValueStr, sizeof(fdwValueStr), settings.calFirstDayMonday ? "Monday" : "Sunday");
        C2D_Text tFdwValue; C2D_TextParse(&tFdwValue, buf, fdwValueStr); C2D_TextOptimize(&tFdwValue);
        float fdwvw, fdwvh;
        C2D_TextGetDimensions(&tFdwValue, 0.44f, 0.44f, &fdwvw, &fdwvh);
        C2D_DrawText(&tFdwValue, C2D_WithColor, 295.0f-fdwvw, 55+(32-fdwvh)*0.5f, 0.5f, 0.44f, 0.44f, C2D_Color32(150,220,190,255));

        float csbw, csbh;
        C2D_DrawRectSolid(15, 195, 0.0f, 140, 25, colYellow);
        char csBackStr[8]; snprintf(csBackStr, sizeof(csBackStr), "Back");
        C2D_Text tCsBack; C2D_TextParse(&tCsBack, buf, csBackStr); C2D_TextOptimize(&tCsBack);
        C2D_TextGetDimensions(&tCsBack, 0.44f, 0.44f, &csbw, &csbh);
        C2D_DrawText(&tCsBack, C2D_WithColor, 15+(140.0f-csbw)*0.5f, 195.0f+(25.0f-csbh)*0.5f, 0.5f, 0.44f, 0.44f, colTextBlack);
    }
    else if (currentScreen == SCREEN_CALENDAR_MONTHYEAR) {
        C2D_TargetClear(target, C2D_Color32(22, 26, 34, 255));
        drawSlimShadowTitle("JUMP TO", buf, 20, 15, 0.55f, C2D_Color32(150,180,220,255), C2D_Color32(8, 10, 16, 220));

        const float fieldW2 = 100.0f, fieldGap2 = 10.0f;
        const float totalW2 = fieldW2*2 + fieldGap2;
        const float startX2 = 15.0f + (290.0f - totalW2) * 0.5f;
        float monthFieldX = startX2;
        float yearFieldX  = monthFieldX + fieldW2 + fieldGap2;

        static const char* cmyMonthNames[12] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
        float cmytw, cmyth;

        C2D_DrawRectSolid(monthFieldX, 84, 0.0f, fieldW2, 50, C2D_Color32(45, 55, 65, 255));
        char cmyMonthStr[16]; snprintf(cmyMonthStr, sizeof(cmyMonthStr), "%s", cmyMonthNames[calEditMonth]);
        C2D_Text tCmyMonth; C2D_TextParse(&tCmyMonth, buf, cmyMonthStr); C2D_TextOptimize(&tCmyMonth);
        C2D_TextGetDimensions(&tCmyMonth, 0.42f, 0.42f, &cmytw, &cmyth);
        C2D_DrawText(&tCmyMonth, C2D_WithColor, monthFieldX+(fieldW2-cmytw)*0.5f, 84+(50.0f-cmyth)*0.5f, 0.5f, 0.42f, 0.42f, colTextWhite);

        // Frecce +/- per mese
        char cplusStr[2] = "+"; C2D_Text tCplus; C2D_TextParse(&tCplus, buf, cplusStr); C2D_TextOptimize(&tCplus);
        float cplusw, cplush; C2D_TextGetDimensions(&tCplus, 0.5f, 0.5f, &cplusw, &cplush);
        C2D_DrawRectSolid(monthFieldX, 62, 0.0f, fieldW2, 20, C2D_Color32(80, 65, 60, 255));
        C2D_DrawText(&tCplus,  C2D_WithColor, monthFieldX+(fieldW2-cplusw)*0.5f, 62+(20.0f-cplush)*0.5f, 0.5f, 0.5f, 0.5f, colTextWhite);
        char cminusStr[2] = "-"; C2D_Text tCminus; C2D_TextParse(&tCminus, buf, cminusStr); C2D_TextOptimize(&tCminus);
        float cminusw, cminush; C2D_TextGetDimensions(&tCminus, 0.5f, 0.5f, &cminusw, &cminush);
        C2D_DrawRectSolid(monthFieldX, 136, 0.0f, fieldW2, 20, C2D_Color32(80, 65, 60, 255));
        C2D_DrawText(&tCminus, C2D_WithColor, monthFieldX+(fieldW2-cminusw)*0.5f, 136+(20.0f-cminush)*0.5f, 0.5f, 0.5f, 0.5f, colTextWhite);

        C2D_DrawRectSolid(yearFieldX, 84, 0.0f, fieldW2, 50, C2D_Color32(45, 55, 65, 255));
        char cmyYearStr[8]; snprintf(cmyYearStr, sizeof(cmyYearStr), "%d", calEditYear);
        C2D_Text tCmyYear; C2D_TextParse(&tCmyYear, buf, cmyYearStr); C2D_TextOptimize(&tCmyYear);
        C2D_TextGetDimensions(&tCmyYear, 0.52f, 0.52f, &cmytw, &cmyth);
        C2D_DrawText(&tCmyYear, C2D_WithColor, yearFieldX+(fieldW2-cmytw)*0.5f, 84+(50.0f-cmyth)*0.5f, 0.5f, 0.52f, 0.52f, colTextWhite);
        C2D_DrawRectSolid(yearFieldX, 62, 0.0f, fieldW2, 20, C2D_Color32(80, 65, 60, 255));
        C2D_DrawText(&tCplus,  C2D_WithColor, yearFieldX+(fieldW2-cplusw)*0.5f,  62+(20.0f-cplush)*0.5f,  0.5f, 0.5f, 0.5f, colTextWhite);
        C2D_DrawRectSolid(yearFieldX, 136, 0.0f, fieldW2, 20, C2D_Color32(80, 65, 60, 255));
        C2D_DrawText(&tCminus, C2D_WithColor, yearFieldX+(fieldW2-cminusw)*0.5f, 136+(20.0f-cminush)*0.5f, 0.5f, 0.5f, 0.5f, colTextWhite);

        float cmybw, cmybh;
        C2D_DrawRectSolid(15, 195, 0.0f, 90, 25, colYellow);
        char cmyGoStr[4]; snprintf(cmyGoStr, sizeof(cmyGoStr), "Go");
        C2D_Text tCmyGo; C2D_TextParse(&tCmyGo, buf, cmyGoStr); C2D_TextOptimize(&tCmyGo);
        C2D_TextGetDimensions(&tCmyGo, 0.55f, 0.55f, &cmybw, &cmybh);
        C2D_DrawText(&tCmyGo, C2D_WithColor, 15+(90.0f-cmybw)*0.5f, 195.0f+(25.0f-cmybh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
        C2D_DrawRectSolid(115, 195, 0.0f, 90, 25, colYellow);
        char cmyNowStr[4]; snprintf(cmyNowStr, sizeof(cmyNowStr), "Now");
        C2D_Text tCmyNow; C2D_TextParse(&tCmyNow, buf, cmyNowStr); C2D_TextOptimize(&tCmyNow);
        C2D_TextGetDimensions(&tCmyNow, 0.55f, 0.55f, &cmybw, &cmybh);
        C2D_DrawText(&tCmyNow, C2D_WithColor, 115+(90.0f-cmybw)*0.5f, 195.0f+(25.0f-cmybh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
        C2D_DrawRectSolid(215, 195, 0.0f, 90, 25, colYellow);
        char cmyCancelStr[8]; snprintf(cmyCancelStr, sizeof(cmyCancelStr), "Back");
        C2D_Text tCmyCancel; C2D_TextParse(&tCmyCancel, buf, cmyCancelStr); C2D_TextOptimize(&tCmyCancel);
        C2D_TextGetDimensions(&tCmyCancel, 0.55f, 0.55f, &cmybw, &cmybh);
        C2D_DrawText(&tCmyCancel, C2D_WithColor, 215+(90.0f-cmybw)*0.5f, 195.0f+(25.0f-cmybh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);
    }
    else if (currentScreen == SCREEN_DIARY_ENTRY) {
        C2D_TargetClear(target, C2D_Color32(24, 28, 22, 255));

        static const char* deMonthNames[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        char deTitleStr[32];
        snprintf(deTitleStr, sizeof(deTitleStr), "%d %s %d", diaryViewDay, deMonthNames[diaryViewMonth >= 0 ? diaryViewMonth : 0], diaryViewYear);
        C2D_Text tDeTitle; C2D_TextParse(&tDeTitle, buf, deTitleStr); C2D_TextOptimize(&tDeTitle);
        C2D_DrawText(&tDeTitle, C2D_WithColor, 20, 15, 0.5f, 0.6f, 0.6f, C2D_Color32(220,200,150,255));

        char dePrevStr[2] = "<"; C2D_Text tDePrev; C2D_TextParse(&tDePrev, buf, dePrevStr); C2D_TextOptimize(&tDePrev);
        float dpw, dph; C2D_TextGetDimensions(&tDePrev, 0.6f, 0.6f, &dpw, &dph);
        C2D_DrawText(&tDePrev, C2D_WithColor, 250.0f - dpw*0.5f, 12, 0.5f, 0.6f, 0.6f, C2D_Color32(220,200,150,255));
        char deNextStr[2] = ">"; C2D_Text tDeNext; C2D_TextParse(&tDeNext, buf, deNextStr); C2D_TextOptimize(&tDeNext);
        float dnw, dnh; C2D_TextGetDimensions(&tDeNext, 0.6f, 0.6f, &dnw, &dnh);
        C2D_DrawText(&tDeNext, C2D_WithColor, 288.0f - dnw*0.5f, 12, 0.5f, 0.6f, 0.6f, C2D_Color32(220,200,150,255));

        int deIdx = findDiaryEntry(diaryViewYear, diaryViewMonth, diaryViewDay);
        C2D_DrawRectSolid(15, 50, 0.0f, 290, 130, C2D_Color32(40, 46, 38, 255));
        char deTextStr[DIARY_TEXT_MAX + 8];
        if (deIdx >= 0 && diaryEntries[deIdx].text[0])
            snprintf(deTextStr, sizeof(deTextStr), "%s", diaryEntries[deIdx].text);
        else
            deTextStr[0] = '\0';
        {
            static char deLines[WRAP_MAX_LINES][WRAP_LINE_LEN];
            int deLineCount = wrapTextIntoLines(deTextStr, 270.0f, 0.38f, buf, deLines);
            diaryTotalLines = deLineCount;
            float deLineY = 58.0f;
            for (int li = diaryScrollOffset; li < deLineCount && li < diaryScrollOffset + DIARY_VISIBLE_LINES; li++) {
                C2D_Text tDeLine; C2D_TextParse(&tDeLine, buf, deLines[li]); C2D_TextOptimize(&tDeLine);
                C2D_DrawText(&tDeLine, C2D_WithColor, 22, deLineY, 0.5f, 0.38f, 0.38f, C2D_Color32(210,210,200,255));
                deLineY += 15.0f;
            }
            if (deLineCount > DIARY_VISIBLE_LINES) {
                bool arrowVisible = fmodf(globalAnimTime, 1.0f) < 0.6f;
                if (arrowVisible) {
                    char scrollArrowStr[2] = "v";
                    C2D_Text tScrollArrow; C2D_TextParse(&tScrollArrow, buf, scrollArrowStr); C2D_TextOptimize(&tScrollArrow);
                    C2D_DrawText(&tScrollArrow, C2D_WithColor, 288, 181, 0.5f, 0.42f, 0.42f, C2D_Color32(230,200,120,255));
                }
            }
        }
        float debw, debh;
        C2D_DrawRectSolid(15, 195, 0.0f, 130, 25, colYellow);
        char deWriteStr[8]; snprintf(deWriteStr, sizeof(deWriteStr), "Write");
        C2D_Text tDeWrite; C2D_TextParse(&tDeWrite, buf, deWriteStr); C2D_TextOptimize(&tDeWrite);
        C2D_TextGetDimensions(&tDeWrite, 0.44f, 0.44f, &debw, &debh);
        C2D_DrawText(&tDeWrite, C2D_WithColor, 15+(130.0f-debw)*0.5f, 195.0f+(25.0f-debh)*0.5f, 0.5f, 0.44f, 0.44f, colTextBlack);
        C2D_DrawRectSolid(175, 195, 0.0f, 130, 25, colYellow);
        char deBackStr[8]; snprintf(deBackStr, sizeof(deBackStr), "Back");
        C2D_Text tDeBack; C2D_TextParse(&tDeBack, buf, deBackStr); C2D_TextOptimize(&tDeBack);
        C2D_TextGetDimensions(&tDeBack, 0.44f, 0.44f, &debw, &debh);
        C2D_DrawText(&tDeBack, C2D_WithColor, 175+(130.0f-debw)*0.5f, 195.0f+(25.0f-debh)*0.5f, 0.5f, 0.44f, 0.44f, colTextBlack);
    }
    else if (currentScreen == SCREEN_NOTES_LIST) {
        C2D_TargetClear(target, C2D_Color32(26, 24, 30, 255));
        drawSlimShadowTitle("NOTES", buf, 20, 12, 0.6f, C2D_Color32(200,180,220,255), C2D_Color32(10, 8, 14, 220));

        float nlnw, nlnh;
        C2D_DrawRectSolid(165, 8, 0.0f, 70, 24, colYellow);
        char nlNewStr[4]; snprintf(nlNewStr, sizeof(nlNewStr), "New");
        C2D_Text tNlNew; C2D_TextParse(&tNlNew, buf, nlNewStr); C2D_TextOptimize(&tNlNew);
        C2D_TextGetDimensions(&tNlNew, 0.55f, 0.55f, &nlnw, &nlnh);
        C2D_DrawText(&tNlNew, C2D_WithColor, 165+(70.0f-nlnw)*0.5f, 8+(24.0f-nlnh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        C2D_DrawRectSolid(240, 8, 0.0f, 65, 24, colYellow);
        char nlBackStr[8]; snprintf(nlBackStr, sizeof(nlBackStr), "Back");
        C2D_Text tNlBack; C2D_TextParse(&tNlBack, buf, nlBackStr); C2D_TextOptimize(&tNlBack);
        float nlbw, nlbh;
        C2D_TextGetDimensions(&tNlBack, 0.55f, 0.55f, &nlbw, &nlbh);
        C2D_DrawText(&tNlBack, C2D_WithColor, 240+(65.0f-nlbw)*0.5f, 8+(24.0f-nlbh)*0.5f, 0.5f, 0.55f, 0.55f, colTextBlack);

        static int nlOrder[NOTE_MAX_ENTRIES];
        int nlCount = getNotesOrderedByModified(nlOrder, NOTE_MAX_ENTRIES);
        const float nlCardX = 20.0f, nlCardW = 280.0f, nlCardH = 32.0f, nlCardGap = 8.0f;
        const float nlCardStep = nlCardH + nlCardGap;
        const float nlListTopY = 42.0f;
        int nlMaxScroll = (nlCount > NOTES_LIST_VISIBLE_COUNT) ? (nlCount - NOTES_LIST_VISIBLE_COUNT) : 0;
        if (notesListScrollOffset > nlMaxScroll) notesListScrollOffset = nlMaxScroll;
        if (notesListScrollOffset < 0) notesListScrollOffset = 0;
        int nlVisibleCount = 0;
        for (int ni = notesListScrollOffset; ni < nlCount && nlVisibleCount < NOTES_LIST_VISIBLE_COUNT; ni++) {
            float cardY = nlListTopY + nlVisibleCount * nlCardStep;
            int idx = nlOrder[ni];
            bool isSelected = (notesListScrollOffset + nlVisibleCount == notesListSelectedIndex);
            u32 cardColor = isSelected ? C2D_Color32(95, 80, 115, 255) : C2D_Color32(48, 44, 58, 255);
            C2D_DrawRectSolid(nlCardX, cardY, 0.0f, nlCardW, nlCardH, cardColor);
            char nlTitleLine[48];
            getNoteTitleLine(idx, nlTitleLine, sizeof(nlTitleLine));
            C2D_Text tNlLine; C2D_TextParse(&tNlLine, buf, nlTitleLine); C2D_TextOptimize(&tNlLine);
            float nltw, nlth;
            C2D_TextGetDimensions(&tNlLine, 0.38f, 0.38f, &nltw, &nlth);
            C2D_DrawText(&tNlLine, C2D_WithColor, nlCardX + 10.0f, cardY + (nlCardH - nlth) * 0.5f, 0.5f, 0.38f, 0.38f, colTextWhite);
            nlVisibleCount++;
        }
        if (nlCount > notesListScrollOffset + NOTES_LIST_VISIBLE_COUNT) {
            bool nlArrowVisible = fmodf(globalAnimTime, 1.0f) < 0.6f;
            if (nlArrowVisible) {
                char nlScrollArrowStr[2] = "v";
                C2D_Text tNlScrollArrow; C2D_TextParse(&tNlScrollArrow, buf, nlScrollArrowStr); C2D_TextOptimize(&tNlScrollArrow);
                C2D_DrawText(&tNlScrollArrow, C2D_WithColor, 288, 207, 0.5f, 0.42f, 0.42f, colYellow);
            }
        }
        if (nlCount == 0) {
            char nlEmptyStr[32]; snprintf(nlEmptyStr, sizeof(nlEmptyStr), "No notes yet. Tap New.");
            C2D_Text tNlEmpty; C2D_TextParse(&tNlEmpty, buf, nlEmptyStr); C2D_TextOptimize(&tNlEmpty);
            C2D_DrawText(&tNlEmpty, C2D_WithColor, 20, 55, 0.5f, 0.40f, 0.40f, C2D_Color32(180,180,190,255));
        }
    }
    else if (currentScreen == SCREEN_NOTE_VIEW) {
        C2D_TargetClear(target, C2D_Color32(26, 24, 30, 255));

        char nvTitleLine[48];
        getNoteTitleLine(noteViewIdx, nvTitleLine, sizeof(nvTitleLine));
        C2D_Text tNvTitle; C2D_TextParse(&tNvTitle, buf, nvTitleLine); C2D_TextOptimize(&tNvTitle);
        C2D_DrawText(&tNvTitle, C2D_WithColor, 20, 12, 0.5f, 0.5f, 0.5f, C2D_Color32(220,200,150,255));

        C2D_DrawRectSolid(15, 42, 0.0f, 290, 138, C2D_Color32(40, 38, 46, 255));
        char nvTextStr[NOTE_TEXT_MAX + 8];
        if (noteViewIdx >= 0 && notes[noteViewIdx].text[0])
            snprintf(nvTextStr, sizeof(nvTextStr), "%s", notes[noteViewIdx].text);
        else
            snprintf(nvTextStr, sizeof(nvTextStr), "Empty. Tap here to write.");
        {
            static char nvLines[WRAP_MAX_LINES][WRAP_LINE_LEN];
            int nvLineCount = wrapTextIntoLines(nvTextStr, 270.0f, 0.38f, buf, nvLines);
            noteTotalLines = nvLineCount;
            float nvLineY = 50.0f;
            for (int li = noteScrollOffset; li < nvLineCount && li < noteScrollOffset + NOTE_VISIBLE_LINES; li++) {
                C2D_Text tNvLine; C2D_TextParse(&tNvLine, buf, nvLines[li]); C2D_TextOptimize(&tNvLine);
                C2D_DrawText(&tNvLine, C2D_WithColor, 22, nvLineY, 0.5f, 0.38f, 0.38f, C2D_Color32(215,210,220,255));
                nvLineY += 15.0f;
            }
            if (nvLineCount > NOTE_VISIBLE_LINES) {
                bool nvArrowVisible = fmodf(globalAnimTime, 1.0f) < 0.6f;
                if (nvArrowVisible) {
                    char nvScrollArrowStr[2] = "v";
                    C2D_Text tNvScrollArrow; C2D_TextParse(&tNvScrollArrow, buf, nvScrollArrowStr); C2D_TextOptimize(&tNvScrollArrow);
                    C2D_DrawText(&tNvScrollArrow, C2D_WithColor, 288, 177, 0.5f, 0.42f, 0.42f, C2D_Color32(230,200,120,255));
                }
            }
        }
        float nvbw, nvbh;
        C2D_DrawRectSolid(15, 195, 0.0f, 90, 25, colYellow);
        char nvWriteStr[8]; snprintf(nvWriteStr, sizeof(nvWriteStr), "Write");
        C2D_Text tNvWrite; C2D_TextParse(&tNvWrite, buf, nvWriteStr); C2D_TextOptimize(&tNvWrite);
        C2D_TextGetDimensions(&tNvWrite, 0.42f, 0.42f, &nvbw, &nvbh);
        C2D_DrawText(&tNvWrite, C2D_WithColor, 15+(90.0f-nvbw)*0.5f, 195.0f+(25.0f-nvbh)*0.5f, 0.5f, 0.42f, 0.42f, colTextBlack);
        C2D_DrawRectSolid(115, 195, 0.0f, 90, 25, C2D_Color32(210, 70, 70, 255));
        char nvDeleteStr[8]; snprintf(nvDeleteStr, sizeof(nvDeleteStr), "Delete");
        C2D_Text tNvDelete; C2D_TextParse(&tNvDelete, buf, nvDeleteStr); C2D_TextOptimize(&tNvDelete);
        C2D_TextGetDimensions(&tNvDelete, 0.42f, 0.42f, &nvbw, &nvbh);
        C2D_DrawText(&tNvDelete, C2D_WithColor, 115+(90.0f-nvbw)*0.5f, 195.0f+(25.0f-nvbh)*0.5f, 0.5f, 0.42f, 0.42f, colTextWhite);
        C2D_DrawRectSolid(215, 195, 0.0f, 90, 25, colYellow);
        char nvBackStr[8]; snprintf(nvBackStr, sizeof(nvBackStr), "Back");
        C2D_Text tNvBack; C2D_TextParse(&tNvBack, buf, nvBackStr); C2D_TextOptimize(&tNvBack);
        C2D_TextGetDimensions(&tNvBack, 0.42f, 0.42f, &nvbw, &nvbh);
        C2D_DrawText(&tNvBack, C2D_WithColor, 215+(90.0f-nvbw)*0.5f, 195.0f+(25.0f-nvbh)*0.5f, 0.5f, 0.42f, 0.42f, colTextBlack);

        if (noteDeleteConfirmPending) {
            C2D_DrawRectSolid(0, 0, 0.8f, 320, 240, C2D_Color32(0, 0, 0, 200));
            char nvConfirmStr[24]; snprintf(nvConfirmStr, sizeof(nvConfirmStr), "Delete this note?");
            C2D_Text tNvConfirm; C2D_TextParse(&tNvConfirm, buf, nvConfirmStr); C2D_TextOptimize(&tNvConfirm);
            float ncw, nch;
            C2D_TextGetDimensions(&tNvConfirm, 0.55f, 0.55f, &ncw, &nch);
            C2D_DrawText(&tNvConfirm, C2D_WithColor, (320.0f-ncw)*0.5f, 90, 0.85f, 0.55f, 0.55f, C2D_Color32(255,255,255,255));
            C2D_DrawRectSolid(70, 140, 0.85f, 80, 32, C2D_Color32(210, 70, 70, 255));
            char nvYesStr[4]; snprintf(nvYesStr, sizeof(nvYesStr), "Yes");
            C2D_Text tNvYes; C2D_TextParse(&tNvYes, buf, nvYesStr); C2D_TextOptimize(&tNvYes);
            float nyw, nyh;
            C2D_TextGetDimensions(&tNvYes, 0.5f, 0.5f, &nyw, &nyh);
            C2D_DrawText(&tNvYes, C2D_WithColor, 70+(80.0f-nyw)*0.5f, 140+(32.0f-nyh)*0.5f, 0.85f, 0.5f, 0.5f, colTextWhite);
            C2D_DrawRectSolid(170, 140, 0.85f, 80, 32, colYellow);
            char nvNoStr[4]; snprintf(nvNoStr, sizeof(nvNoStr), "No");
            C2D_Text tNvNo; C2D_TextParse(&tNvNo, buf, nvNoStr); C2D_TextOptimize(&tNvNo);
            float nnw, nnh;
            C2D_TextGetDimensions(&tNvNo, 0.5f, 0.5f, &nnw, &nnh);
            C2D_DrawText(&tNvNo, C2D_WithColor, 170+(80.0f-nnw)*0.5f, 140+(32.0f-nnh)*0.5f, 0.85f, 0.5f, 0.5f, colTextBlack);
        }
    }

    if (alarmRinging) {
        // Overlay a schermo intero, completamente opaco: nasconde del tutto la schermata
        // sottostante mentre la sveglia suona, invitando a toccare per fermarla.
        float pulse = (sinf(alarmFlashPhase) * 0.5f) + 0.5f;
        u8 popR = (u8)(180.0f + pulse * 60.0f);
        u32 popupBg = C2D_Color32(popR, 25, 20, 255);

        C2D_TargetClear(target, popupBg);

        char popStr[40] = "Tap to stop alarm";
        C2D_Text tPopup; C2D_TextParse(&tPopup, buf, popStr); C2D_TextOptimize(&tPopup);
        float pw, ph;
        C2D_TextGetDimensions(&tPopup, 0.9f, 0.9f, &pw, &ph);
        C2D_DrawText(&tPopup, C2D_WithColor, (320.0f-pw)*0.5f, (240.0f-ph)*0.5f, 0.5f, 0.9f, 0.9f, C2D_Color32(255,255,255,255));
    }
}

/* -------------------- INPUT -------------------- */
static int updateInput() {
    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();

    if (kDown & KEY_A) {
        if (alarmRinging) {
            stopAlarmRinging();
            return 0;
        }
    }

    if (kDown & KEY_B) {
        if (currentScreen == SCREEN_SETTINGS) {
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
        else if (currentScreen == SCREEN_ALARM) {
            currentScreen = SCREEN_SETTINGS;
            return 0;
        }
        else if (currentScreen == SCREEN_TIMER) {
            currentScreen = SCREEN_SETTINGS;
            return 0;
        }
        else if (currentScreen == SCREEN_CALENDAR) {
            currentScreen = SCREEN_MAIN;
            return 0;
        }
        else if (currentScreen == SCREEN_CALENDAR_SETTINGS) {
            currentScreen = SCREEN_CALENDAR;
            return 0;
        }
        else if (currentScreen == SCREEN_DIARY_ENTRY) {
            currentScreen = SCREEN_CALENDAR;
            return 0;
        }
        else if (currentScreen == SCREEN_CALENDAR_MONTHYEAR) {
            currentScreen = SCREEN_CALENDAR;
            return 0;
        }
        else if (currentScreen == SCREEN_NOTES_LIST) {
            currentScreen = SCREEN_MAIN;
            return 0;
        }
        else if (currentScreen == SCREEN_NOTE_VIEW) {
            if (noteDeleteConfirmPending) { noteDeleteConfirmPending = false; return 0; }
            currentScreen = SCREEN_NOTES_LIST;
            return 0;
        }
        else if (currentScreen == SCREEN_STATS) {
            currentScreen = SCREEN_MAIN;
            return 0;
        }
        else if (currentScreen == SCREEN_MANUAL) {
            currentScreen = SCREEN_RECORDS;
            return 0;
        }
    }

    if (currentScreen == SCREEN_NOTE_VIEW && !noteDeleteConfirmPending) {
        int noteMaxScroll = (noteTotalLines > NOTE_VISIBLE_LINES) ? (noteTotalLines - NOTE_VISIBLE_LINES) : 0;
        bool noteScrollUp = false, noteScrollDown = false;
        if (kDown & KEY_DUP)   noteScrollUp   = true;
        if (kDown & KEY_DDOWN) noteScrollDown = true;
        circlePosition noteCirclePos;
        hidCircleRead(&noteCirclePos);
        const s16 noteDeadzone = 40;
        if (noteCirclePos.dy > noteDeadzone || noteCirclePos.dy < -noteDeadzone) {
            noteScrollRepeatTimer += 0.0166f;
            if (noteScrollRepeatTimer >= 0.12f) {
                noteScrollRepeatTimer = 0.0f;
                if (noteCirclePos.dy > noteDeadzone) noteScrollUp = true;
                else noteScrollDown = true;
            }
        } else { noteScrollRepeatTimer = 0.0f; }
        if (noteScrollUp   && noteScrollOffset > 0)            noteScrollOffset--;
        if (noteScrollDown && noteScrollOffset < noteMaxScroll) noteScrollOffset++;
    }
    if (currentScreen == SCREEN_NOTES_LIST) {
        static int nlOrderScroll[NOTE_MAX_ENTRIES];
        int nlCountScroll = getNotesOrderedByModified(nlOrderScroll, NOTE_MAX_ENTRIES);
        if (notesListSelectedIndex >= nlCountScroll) notesListSelectedIndex = (nlCountScroll > 0) ? nlCountScroll - 1 : 0;
        if (notesListSelectedIndex < 0) notesListSelectedIndex = 0;
        bool nlScrollUp = false, nlScrollDown = false;
        if (kDown & KEY_DUP)   nlScrollUp   = true;
        if (kDown & KEY_DDOWN) nlScrollDown = true;
        circlePosition nlCirclePos;
        hidCircleRead(&nlCirclePos);
        const s16 nlDeadzone = 40;
        if (nlCirclePos.dy > nlDeadzone || nlCirclePos.dy < -nlDeadzone) {
            notesListScrollRepeatTimer += 0.0166f;
            if (notesListScrollRepeatTimer >= 0.12f) {
                notesListScrollRepeatTimer = 0.0f;
                if (nlCirclePos.dy > nlDeadzone) nlScrollUp = true;
                else nlScrollDown = true;
            }
        } else { notesListScrollRepeatTimer = 0.0f; }
        if (nlScrollUp   && notesListSelectedIndex > 0) notesListSelectedIndex--;
        if (nlScrollDown && notesListSelectedIndex < nlCountScroll - 1) notesListSelectedIndex++;
        if (notesListSelectedIndex < notesListScrollOffset) notesListScrollOffset = notesListSelectedIndex;
        if (notesListSelectedIndex >= notesListScrollOffset + NOTES_LIST_VISIBLE_COUNT)
            notesListScrollOffset = notesListSelectedIndex - NOTES_LIST_VISIBLE_COUNT + 1;
        if ((kDown & KEY_A) && nlCountScroll > 0) {
            noteViewIdx = nlOrderScroll[notesListSelectedIndex];
            noteScrollOffset = 0;
            currentScreen = SCREEN_NOTE_VIEW;
        }
    }
    if (currentScreen == SCREEN_DIARY_ENTRY && !alarmRinging) {
        int diaryMaxScroll = (diaryTotalLines > DIARY_VISIBLE_LINES) ? (diaryTotalLines - DIARY_VISIBLE_LINES) : 0;
        bool dScrollUp = false, dScrollDown = false;
        if (kDown & KEY_DUP)   dScrollUp   = true;
        if (kDown & KEY_DDOWN) dScrollDown = true;
        circlePosition dCirclePos;
        hidCircleRead(&dCirclePos);
        const s16 dDeadzone = 40;
        if (dCirclePos.dy > dDeadzone || dCirclePos.dy < -dDeadzone) {
            diaryScrollRepeatTimer += 0.0166f;
            if (diaryScrollRepeatTimer >= 0.12f) {
                diaryScrollRepeatTimer = 0.0f;
                if (dCirclePos.dy > dDeadzone) dScrollUp = true;
                else dScrollDown = true;
            }
        } else { diaryScrollRepeatTimer = 0.0f; }
        if (dScrollUp   && diaryScrollOffset > 0)             diaryScrollOffset--;
        if (dScrollDown && diaryScrollOffset < diaryMaxScroll) diaryScrollOffset++;
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

    // L tenuto + analogico: sposta il blocco orologio/data sul top screen.
    // Il clamp X dinamico viene applicato in drawClock (dipende dalla larghezza del testo).
    // A tenuto insieme a L: resetta la posizione al centro.
    if (kHeld & KEY_TOUCH) {
        touchPosition holdTouch;
        hidTouchRead(&holdTouch);
        HoldTarget target = getHoldTargetAtTouch(holdTouch.px, holdTouch.py);
        if (target != HOLD_NONE) {
            if (target != currentHoldTarget) {
                currentHoldTarget = target;
                holdTimer = 0.0f;
                holdFirstTickDone = false;
            } else {
                holdTimer += 0.0166f;
                float threshold = holdFirstTickDone ? HOLD_REPEAT_INTERVAL : HOLD_INITIAL_DELAY;
                if (holdTimer >= threshold) {
                    holdTimer -= threshold;
                    holdFirstTickDone = true;
                    applyHoldTargetIncrement(target);
                }
            }
        } else {
            currentHoldTarget = HOLD_NONE;
            holdFirstTickDone = false;
            holdTimer = 0.0f;
        }
    } else {
        currentHoldTarget = HOLD_NONE;
        holdFirstTickDone = false;
        holdTimer = 0.0f;
    }

    if (kHeld & KEY_L) {
        circlePosition circlePos;
        hidCircleRead(&circlePos);
        const s16 deadzone = 12;
        if (circlePos.dx > deadzone || circlePos.dx < -deadzone) {
            settings.clockOffsetX += (float)circlePos.dx / 60.0f;
            settingsDirty = true;
        }
        if (circlePos.dy > deadzone || circlePos.dy < -deadzone) {
            settings.clockOffsetY -= (float)circlePos.dy / 60.0f;
            settingsDirty = true;
        }
        if (settings.clockOffsetY < -110.0f) settings.clockOffsetY = -110.0f;
        if (settings.clockOffsetY >   97.0f) settings.clockOffsetY =   97.0f;
        if (kDown & KEY_A) {
            settings.clockOffsetX = 0.0f;
            settings.clockOffsetY = 0.0f;
            settingsDirty = true;
            return 0;
        }
    }

    if (kDown & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);

        if (alarmRinging) {
            // L'overlay ora copre tutto il bottom screen (vedi drawBottomScreen),
            // quindi qualsiasi tap mentre suona ferma la sveglia.
            stopAlarmRinging();
            return 0;
        }

        if (lowerScreenOff) {
            lowerScreenOff = false;
            return 0; 
        }

        if (currentScreen == SCREEN_MAIN) {
            if (touch.px >= 15 && touch.px <= 305 && touch.py >= 12 && touch.py <= 45)
                playTestBeep();

            // Testi alarm/timer tappabili: aprono la schermata corrispondente
            if (mainAlarmTextVisible && touch.px >= mainAlarmTextX0 && touch.px <= mainAlarmTextX1 && touch.py >= mainAlarmTextY0 && touch.py <= mainAlarmTextY1)
                currentScreen = SCREEN_ALARM;
            if (mainTimerTextVisible && touch.px >= mainTimerTextX0 && touch.px <= mainTimerTextX1 && touch.py >= mainTimerTextY0 && touch.py <= mainTimerTextY1)
                currentScreen = SCREEN_TIMER;

            // Riga superiore: Notes (sinistra), Calendar (destra)
            if (touch.px >= 15 && touch.px <= 105 && touch.py >= 148 && touch.py <= 180) {
                notesListScrollOffset = 0;
                notesListSelectedIndex = 0;
                currentScreen = SCREEN_NOTES_LIST;
            }
            if (touch.px >= 215 && touch.px <= 305 && touch.py >= 148 && touch.py <= 180) {
                calDisplayMonth = -1; // forza il reset al mese corrente all'ingresso
                currentScreen = SCREEN_CALENDAR;
            }

            // Riga inferiore: Settings, Screen Off, Manual
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
            if (touch.px >= 15 && touch.px <= 69 && touch.py >= 190 && touch.py <= 220) {
                currentScreen = SCREEN_MAIN;
            }
            if (touch.px >= 74 && touch.px <= 128 && touch.py >= 190 && touch.py <= 220) {
                currentScreen = SCREEN_CREDITS;
            }
            if (touch.px >= 133 && touch.px <= 187 && touch.py >= 190 && touch.py <= 220) {
                if (alarmCfg.enabled) {
                    // Sveglia già impostata: popola i campi con i valori salvati,
                    // convertendo da 24h (storage interno) al formato di visualizzazione corrente.
                    alarmEditMinute = alarmCfg.minute;
                    if (!settings.timeFormat24h) {
                        u32 h12 = alarmCfg.hour % 12; if (h12 == 0) h12 = 12;
                        alarmEditHour = h12;
                        alarmEditPM   = (alarmCfg.hour >= 12);
                    } else {
                        alarmEditHour = alarmCfg.hour;
                        alarmEditPM   = false;
                    }
                } else {
                    // Nessuna sveglia impostata: parti da zero (o 12:00 AM in formato 12h)
                    alarmEditMinute = 0;
                    alarmEditPM     = false;
                    alarmEditHour   = settings.timeFormat24h ? 0 : 12;
                }
                currentScreen = SCREEN_ALARM;
            }
            if (touch.px >= 192 && touch.px <= 246 && touch.py >= 190 && touch.py <= 220) {
                if (timerState == TIMER_IDLE) {
                    // Mostra l'ultima durata impostata (se presente) così l'utente può ripartire da lì,
                    // altrimenti i campi restano a quanto erano lasciati l'ultima volta in questa sessione.
                    timerEditHour   = timerTotalSeconds / 3600;
                    timerEditMinute = (timerTotalSeconds % 3600) / 60;
                    timerEditSecond = timerTotalSeconds % 60;
                }
                currentScreen = SCREEN_TIMER;
            }
            if (touch.px >= 251 && touch.px <= 305 && touch.py >= 190 && touch.py <= 220) {
                settings = (AppSettings){0, 1, 1, 0, 0, 0};
                settingsDirty = true; // salvato solo all'uscita dall'app
            }
        }
        else if (currentScreen == SCREEN_ALARM) {
            bool is12h = !settings.timeFormat24h;

            // Stesse coordinate X usate nel rendering (vedi drawBottomScreen)
            float hourFieldX, minFieldX, ampmFieldX;
            if (is12h) {
                const float gap = 10.0f;
                const float totalW = 80.0f + gap + 80.0f + gap + 66.0f;
                const float startX = 15.0f + (290.0f - totalW) * 0.5f;
                hourFieldX = startX;
                minFieldX  = hourFieldX + 80.0f + gap;
                ampmFieldX = minFieldX  + 80.0f + gap;
            } else {
                hourFieldX = 82.0f;
                minFieldX  = 172.0f;
                ampmFieldX = 0.0f;
            }

            // Ore: tap sul campo (centro) o sulle frecce + (y=66) / - (y=151)
            if (touch.px >= hourFieldX && touch.px <= hourFieldX + 80) {
                if (touch.py >= 80 && touch.py <= 150) {
                    if (is12h) alarmEditHour = (alarmEditHour % 12) + 1;
                    else       alarmEditHour = (alarmEditHour + 1) % 24;
                } else if (touch.py >= 66 && touch.py <= 82) {
                    if (is12h) alarmEditHour = (alarmEditHour % 12) + 1;
                    else       alarmEditHour = (alarmEditHour + 1) % 24;
                } else if (touch.py >= 151 && touch.py <= 167) {
                    if (is12h) { alarmEditHour = alarmEditHour - 1; if (alarmEditHour < 1) alarmEditHour = 12; }
                    else       { alarmEditHour = (alarmEditHour + 23) % 24; }
                }
            }
            // Minuti: tap sul campo (centro) o sulle frecce + / -
            if (touch.px >= minFieldX && touch.px <= minFieldX + 80) {
                if (touch.py >= 80 && touch.py <= 150) {
                    alarmEditMinute = (alarmEditMinute + 1) % 60;
                } else if (touch.py >= 66 && touch.py <= 82) {
                    alarmEditMinute = (alarmEditMinute + 1) % 60;
                } else if (touch.py >= 151 && touch.py <= 167) {
                    alarmEditMinute = (alarmEditMinute + 59) % 60;
                }
            }
            // AM/PM: tap sul campo
            if (is12h && touch.px >= ampmFieldX && touch.px <= ampmFieldX + 66 && touch.py >= 91 && touch.py <= 139) {
                alarmEditPM = !alarmEditPM;
            }
            // Tasto Back: torna a Settings senza salvare
            if (touch.px >= 15 && touch.px <= 105 && touch.py >= 190 && touch.py <= 220) {
                currentScreen = SCREEN_SETTINGS;
            }
            // Tasto SET: salva e attiva la sveglia con l'orario impostato, resta su questa schermata
            if (touch.px >= 115 && touch.px <= 205 && touch.py >= 190 && touch.py <= 220) {
                alarmCfg.enabled = 1;
                if (is12h) {
                    // Conversione 12h+AM/PM -> 24h per lo storage interno e il confronto col tempo di sistema
                    u32 h24 = alarmEditHour % 12; // 12 -> 0
                    if (alarmEditPM) h24 += 12;
                    alarmCfg.hour = h24;
                } else {
                    alarmCfg.hour = alarmEditHour;
                }
                alarmCfg.minute  = alarmEditMinute;
                alarmLastTrigHour = -1; // permette il trigger anche se l'orario coincide col minuto corrente
                alarmLastTrigMin  = -1;
                alarmDirty = true; // salvato solo all'uscita dall'app
            }
            // Tasto Clear: disattiva/azzera la sveglia, resta su questa schermata
            if (touch.px >= 215 && touch.px <= 305 && touch.py >= 190 && touch.py <= 220) {
                alarmCfg = (AppAlarm){0, 0, 0};
                alarmDirty = true; // salvato solo all'uscita dall'app
            }
        }
        else if (currentScreen == SCREEN_TIMER) {
            // Stesse coordinate X usate nel rendering (vedi drawBottomScreen)
            const float fieldW = 80.0f, gap = 10.0f;
            const float totalW = fieldW*3 + gap*2;
            const float startX = 15.0f + (290.0f - totalW) * 0.5f;
            float hourFieldX = startX;
            float minFieldX  = hourFieldX + fieldW + gap;
            float secFieldX  = minFieldX  + fieldW + gap;

            bool fieldsEditable = (timerState == TIMER_IDLE);

            // I campi si possono modificare solo quando il timer è IDLE
            if (fieldsEditable) {
                // Ore: tap sul campo (centro) o sulle frecce + / -
                if (touch.px >= hourFieldX && touch.px <= hourFieldX + fieldW) {
                    if ((touch.py >= 80 && touch.py <= 150) || (touch.py >= 66 && touch.py <= 82))
                        timerEditHour = (timerEditHour + 1) % 24;
                    else if (touch.py >= 151 && touch.py <= 167)
                        timerEditHour = (timerEditHour + 23) % 24;
                }
                // Minuti: tap sul campo (centro) o sulle frecce + / -
                if (touch.px >= minFieldX && touch.px <= minFieldX + fieldW) {
                    if ((touch.py >= 80 && touch.py <= 150) || (touch.py >= 66 && touch.py <= 82))
                        timerEditMinute = (timerEditMinute + 1) % 60;
                    else if (touch.py >= 151 && touch.py <= 167)
                        timerEditMinute = (timerEditMinute + 59) % 60;
                }
                // Secondi: tap sul campo (centro) o sulle frecce + / -
                if (touch.px >= secFieldX && touch.px <= secFieldX + fieldW) {
                    if ((touch.py >= 80 && touch.py <= 150) || (touch.py >= 66 && touch.py <= 82))
                        timerEditSecond = (timerEditSecond + 1) % 60;
                    else if (touch.py >= 151 && touch.py <= 167)
                        timerEditSecond = (timerEditSecond + 59) % 60;
                }
            }

            // Tasto Back: torna a Settings (il timer, se in corso, continua a girare in background)
            if (touch.px >= 15 && touch.px <= 105 && touch.py >= 190 && touch.py <= 220) {
                currentScreen = SCREEN_SETTINGS;
            }

            // Tasto centrale: Start (IDLE) / Pausa (RUNNING) / Riprendi (PAUSED)
            if (touch.px >= 115 && touch.px <= 205 && touch.py >= 190 && touch.py <= 220) {
                if (timerState == TIMER_IDLE) {
                    u32 totalSec = timerEditHour*3600 + timerEditMinute*60 + timerEditSecond;
                    if (totalSec > 0) { // non avviare un timer a 00:00:00
                        timerTotalSeconds     = totalSec;
                        timerRemainingSeconds = totalSec;
                        timerFrameAccumulator = 0;
                        timerState = TIMER_RUNNING;
                    }
                } else if (timerState == TIMER_RUNNING) {
                    timerState = TIMER_PAUSED;
                } else { // TIMER_PAUSED
                    timerFrameAccumulator = 0; // riparte puntuale dal secondo intero corrente
                    timerState = TIMER_RUNNING;
                }
            }

            // Tasto destro: Reset (IDLE, azzera i campi) / Stop (RUNNING o PAUSED, annulla il countdown)
            if (touch.px >= 215 && touch.px <= 305 && touch.py >= 190 && touch.py <= 220) {
                if (timerState == TIMER_IDLE) {
                    timerEditHour = timerEditMinute = timerEditSecond = 0;
                } else {
                    timerState = TIMER_IDLE;
                    timerRemainingSeconds = 0;
                }
            }
        }
        else if (currentScreen == SCREEN_RECORDS) {
            // 3 bottoni: Manual (0), Reset Stats (1), Back (2) — stesse coordinate del rendering
            const float bBtnW = 93.0f, bBtnGap = 5.0f;
            float bBtnX[3];
            bBtnX[0] = 15.0f;
            bBtnX[1] = bBtnX[0] + bBtnW + bBtnGap;
            bBtnX[2] = bBtnX[1] + bBtnW + bBtnGap;
            if (touch.py >= 185 && touch.py <= 217) {
                if (touch.px >= bBtnX[0] && touch.px <= bBtnX[0]+bBtnW)
                    currentScreen = SCREEN_MANUAL;
                if (touch.px >= bBtnX[1] && touch.px <= bBtnX[1]+bBtnW) {
                    records       = (AppRecords){0, 0, 0, 0};
                    sessionFrames = 0;
                }
                if (touch.px >= bBtnX[2] && touch.px <= bBtnX[2]+bBtnW)
                    currentScreen = SCREEN_MAIN;
            }
        }
        else if (currentScreen == SCREEN_CREDITS) {
            if (touch.px >= 105 && touch.px <= 215 && touch.py >= 185 && touch.py <= 217)
                currentScreen = SCREEN_SETTINGS; 
        }
        else if (currentScreen == SCREEN_MANUAL) {
            if (touch.px >= 211 && touch.px <= 304 && touch.py >= 185 && touch.py <= 217)
                currentScreen = SCREEN_RECORDS;
        }
        else if (currentScreen == SCREEN_CALENDAR) {
            if (touch.px >= 245 && touch.px <= 305 && touch.py >= 12 && touch.py <= 32)
                currentScreen = SCREEN_MAIN;
            if (touch.px >= 15 && touch.px <= 75 && touch.py >= 12 && touch.py <= 32)
                currentScreen = SCREEN_CALENDAR_SETTINGS;
            if (touch.px >= 76 && touch.px <= 112 && touch.py >= 8 && touch.py <= 38) {
                calDisplayMonth--;
                if (calDisplayMonth < 0) { calDisplayMonth = 11; calDisplayYear--; }
            }
            if (touch.px >= 208 && touch.px <= 244 && touch.py >= 8 && touch.py <= 38) {
                calDisplayMonth++;
                if (calDisplayMonth > 11) { calDisplayMonth = 0; calDisplayYear++; }
            }
            if (touch.px >= 112 && touch.px <= 208 && touch.py >= 8 && touch.py <= 38) {
                calEditMonth = (calDisplayMonth >= 0) ? calDisplayMonth : 0;
                calEditYear  = (calDisplayYear  >= 0) ? calDisplayYear  : 2026;
                currentScreen = SCREEN_CALENDAR_MONTHYEAR;
            }
            if (touch.py >= 54 && touch.py <= 222) {
                int tapMonth = (calDisplayMonth >= 0) ? calDisplayMonth : 0;
                int tapYear  = (calDisplayYear  >= 0) ? calDisplayYear  : 2026;
                int tapDaysInMonth = getDaysInMonth(tapMonth, tapYear);
                int tapFirstWeekdayRaw = getFirstWeekdayOfMonth(tapMonth, tapYear);
                int tapFirstWeekday = settings.calFirstDayMonday ? (tapFirstWeekdayRaw + 6) % 7 : tapFirstWeekdayRaw;
                const float tapCalL = 15.0f, tapCalR = 305.0f;
                const float tapColW = (tapCalR - tapCalL) / 7.0f;
                const float tapGridTopY = 54.0f, tapGridBottomY = 222.0f;
                const float tapRowH = (tapGridBottomY - tapGridTopY) / 6.0f;
                int tapRow = (int)((touch.py - tapGridTopY) / tapRowH);
                int tapCol = (int)((touch.px - tapCalL) / tapColW);
                if (tapRow >= 0 && tapRow < 6 && tapCol >= 0 && tapCol < 7) {
                    int tapDayNum = tapRow * 7 + tapCol - tapFirstWeekday + 1;
                    if (tapDayNum >= 1 && tapDayNum <= tapDaysInMonth) {
                        diaryViewYear  = tapYear;
                        diaryViewMonth = tapMonth;
                        diaryViewDay   = tapDayNum;
                        diaryScrollOffset = 0;
                        currentScreen = SCREEN_DIARY_ENTRY;
                    }
                }
            }
        }
        else if (currentScreen == SCREEN_CALENDAR_SETTINGS) {
            if (touch.px >= 15 && touch.px <= 305 && touch.py >= 55 && touch.py <= 87) {
                settings.calFirstDayMonday = !settings.calFirstDayMonday;
                settingsDirty = true;
            }
            if (touch.px >= 15 && touch.px <= 155 && touch.py >= 195 && touch.py <= 220)
                currentScreen = SCREEN_CALENDAR;
        }
        else if (currentScreen == SCREEN_CALENDAR_MONTHYEAR) {
            // fieldW=100, gap=10, totalW=210, startX = 15+(290-210)*0.5f = 55
            const float cmyMonthX = 55.0f, cmyYearX = 165.0f, cmyFW = 100.0f;
            // + mese/anno (y=62-82), - mese/anno (y=136-156)
            if (touch.py >= 62 && touch.py <= 82) {
                if (touch.px >= cmyMonthX && touch.px <= cmyMonthX+cmyFW)
                    calEditMonth = (calEditMonth + 1) % 12;
                if (touch.px >= cmyYearX && touch.px <= cmyYearX+cmyFW)
                    calEditYear++;
            }
            if (touch.py >= 136 && touch.py <= 156) {
                if (touch.px >= cmyMonthX && touch.px <= cmyMonthX+cmyFW)
                    calEditMonth = (calEditMonth + 11) % 12;
                if (touch.px >= cmyYearX && touch.px <= cmyYearX+cmyFW && calEditYear > 2000)
                    calEditYear--;
            }
            if (touch.px >= 15 && touch.px <= 105 && touch.py >= 195 && touch.py <= 220) {
                calDisplayMonth = calEditMonth;
                calDisplayYear  = calEditYear;
                currentScreen = SCREEN_CALENDAR;
            }
            if (touch.px >= 115 && touch.px <= 205 && touch.py >= 195 && touch.py <= 220) {
                time_t rawNow2 = time(NULL);
                struct tm* tmNow2 = localtime(&rawNow2);
                calDisplayMonth = tmNow2->tm_mon;
                calDisplayYear  = tmNow2->tm_year + 1900;
                currentScreen = SCREEN_CALENDAR;
            }
            if (touch.px >= 215 && touch.px <= 305 && touch.py >= 195 && touch.py <= 220)
                currentScreen = SCREEN_CALENDAR;
        }
        else if (currentScreen == SCREEN_DIARY_ENTRY) {
            if (touch.px >= 235 && touch.px <= 265 && touch.py >= 8 && touch.py <= 35) {
                int ny, nm, nd;
                if (findAdjacentDiaryEntry(diaryViewYear, diaryViewMonth, diaryViewDay, -1, &ny, &nm, &nd)) {
                    diaryViewYear = ny; diaryViewMonth = nm; diaryViewDay = nd;
                    diaryScrollOffset = 0;
                }
            }
            if (touch.px >= 273 && touch.px <= 303 && touch.py >= 8 && touch.py <= 35) {
                int ny, nm, nd;
                if (findAdjacentDiaryEntry(diaryViewYear, diaryViewMonth, diaryViewDay, 1, &ny, &nm, &nd)) {
                    diaryViewYear = ny; diaryViewMonth = nm; diaryViewDay = nd;
                    diaryScrollOffset = 0;
                }
            }
            bool tappedWriteBtn = (touch.px >= 15 && touch.px <= 145 && touch.py >= 195 && touch.py <= 220);
            bool tappedTextArea = (touch.px >= 15 && touch.px <= 305 && touch.py >= 50 && touch.py <= 180);
            if (tappedWriteBtn || tappedTextArea) {
                int deIdx2 = getOrCreateDiaryEntry(diaryViewYear, diaryViewMonth, diaryViewDay);
                if (deIdx2 >= 0) {
                    SwkbdState swkbd;
                    char kbdBuf[DIARY_TEXT_MAX];
                    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, DIARY_TEXT_MAX - 1);
                    swkbdSetFeatures(&swkbd, SWKBD_MULTILINE);
                    swkbdSetHintText(&swkbd, "Write something about this day...");
                    swkbdSetInitialText(&swkbd, diaryEntries[deIdx2].text);
                    SwkbdButton kbdButton = swkbdInputText(&swkbd, kbdBuf, sizeof(kbdBuf));
                    if (kbdButton == SWKBD_BUTTON_CONFIRM) {
                        snprintf(diaryEntries[deIdx2].text, DIARY_TEXT_MAX, "%s", kbdBuf);
                        saveDiary();
                        diaryScrollOffset = 0;
                    }
                }
            }
            if (touch.px >= 175 && touch.px <= 305 && touch.py >= 195 && touch.py <= 220)
                currentScreen = SCREEN_CALENDAR;
        }
        else if (currentScreen == SCREEN_NOTES_LIST) {
            if (touch.px >= 165 && touch.px <= 235 && touch.py >= 8 && touch.py <= 32) {
                int newIdx = createNewNote();
                if (newIdx >= 0) {
                    noteViewIdx = newIdx;
                    noteScrollOffset = 0;
                    currentScreen = SCREEN_NOTE_VIEW;
                }
            }
            if (touch.px >= 240 && touch.px <= 305 && touch.py >= 8 && touch.py <= 32)
                currentScreen = SCREEN_MAIN;
            static int nlOrderTouch[NOTE_MAX_ENTRIES];
            int nlCountTouch = getNotesOrderedByModified(nlOrderTouch, NOTE_MAX_ENTRIES);
            const float nlCardXTouch = 20.0f, nlCardWTouch = 280.0f, nlCardHTouch = 32.0f, nlCardGapTouch = 8.0f;
            const float nlCardStepTouch = nlCardHTouch + nlCardGapTouch;
            const float nlListTopYTouch = 42.0f;
            int nlVisibleTouch = 0;
            for (int ni = notesListScrollOffset; ni < nlCountTouch && nlVisibleTouch < NOTES_LIST_VISIBLE_COUNT; ni++) {
                float cardY = nlListTopYTouch + nlVisibleTouch * nlCardStepTouch;
                if (touch.px >= nlCardXTouch && touch.px <= nlCardXTouch + nlCardWTouch &&
                    touch.py >= cardY && touch.py <= cardY + nlCardHTouch) {
                    noteViewIdx = nlOrderTouch[ni];
                    notesListSelectedIndex = ni;
                    noteScrollOffset = 0;
                    currentScreen = SCREEN_NOTE_VIEW;
                    break;
                }
                nlVisibleTouch++;
            }
        }
        else if (currentScreen == SCREEN_NOTE_VIEW) {
            if (noteDeleteConfirmPending) {
                if (touch.px >= 70 && touch.px <= 150 && touch.py >= 140 && touch.py <= 172) {
                    deleteNote(noteViewIdx);
                    noteDeleteConfirmPending = false;
                    currentScreen = SCREEN_NOTES_LIST;
                }
                if (touch.px >= 170 && touch.px <= 250 && touch.py >= 140 && touch.py <= 172)
                    noteDeleteConfirmPending = false;
            } else {
                bool nvTappedWrite = (touch.px >= 15 && touch.px <= 105 && touch.py >= 195 && touch.py <= 220);
                bool nvTappedText  = (touch.px >= 15 && touch.px <= 305 && touch.py >= 42  && touch.py <= 180);
                if (nvTappedWrite || nvTappedText) {
                    if (noteViewIdx >= 0) {
                        SwkbdState swkbd;
                        char kbdBuf[NOTE_TEXT_MAX];
                        swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, NOTE_TEXT_MAX - 1);
                        swkbdSetFeatures(&swkbd, SWKBD_MULTILINE);
                        swkbdSetHintText(&swkbd, "Write your note...");
                        swkbdSetInitialText(&swkbd, notes[noteViewIdx].text);
                        SwkbdButton kbdButton = swkbdInputText(&swkbd, kbdBuf, sizeof(kbdBuf));
                        if (kbdButton == SWKBD_BUTTON_CONFIRM) {
                            snprintf(notes[noteViewIdx].text, NOTE_TEXT_MAX, "%s", kbdBuf);
                            notes[noteViewIdx].modifiedTime = (u32)time(NULL);
                            saveNotes();
                            noteScrollOffset = 0;
                        }
                    }
                }
                if (touch.px >= 115 && touch.px <= 205 && touch.py >= 195 && touch.py <= 220)
                    noteDeleteConfirmPending = true;
                if (touch.px >= 215 && touch.px <= 305 && touch.py >= 195 && touch.py <= 220)
                    currentScreen = SCREEN_NOTES_LIST;
            }
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
    ptmuInit();
    mcuHwcInit();

    // Lettura iniziale batteria — così il valore è corretto già dal primo frame
    {
        u8 rawBatt = 0;
        if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&rawBatt)))
            batteryPercent = (int)rawBatt;
        PTMU_GetBatteryChargeState(&batteryCharging);
    }
    C2D_Prepare();

    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    C2D_TextBuf topBuf    = C2D_TextBufNew(1024);
    C2D_TextBuf bottomBuf = C2D_TextBufNew(2048);

    // Creiamo la directory di salvataggio una volta sola all'avvio
    mkdir(SAVE_DIR, 0777);

    loadRecords();
    loadSettings();
    loadAlarm();
    loadDiary();
    loadNotes();
    initStars();
    initStaticTexts();
    initAudio();

    int      prevHour  = -1;
    int      prevMin   = -1;
    bool     halfHourTriggered = false;

    while (aptMainLoop()) {
        if (aptShouldJumpToHome()) {
            aptJumpToHomeMenu();
        }

        if (updateInput()) break;

        sessionFrames++;

        if (sessionFrames % 60 == 0) {
            u8 rawBatt = 0;
            if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&rawBatt)))
                batteryPercent = (int)rawBatt;
            PTMU_GetBatteryChargeState(&batteryCharging);
        }

        if (sessionFrames >= 3600) {
            records.totalTimeSeconds += 60;
            sessionFrames -= 3600; // il salvataggio avviene comunque, sempre, all'uscita dall'app
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

        if (alarmCfg.enabled && !alarmRinging &&
            tmv->tm_hour == (int)alarmCfg.hour && tmv->tm_min == (int)alarmCfg.minute &&
            !(alarmLastTrigHour == tmv->tm_hour && alarmLastTrigMin == tmv->tm_min)) {
            startAlarmRinging(false); // sorgente: sveglia
            alarmLastTrigHour = tmv->tm_hour;
            alarmLastTrigMin  = tmv->tm_min;
        }

        updateAlarmRinging();
        updateTimer();

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
        drawTimerCountdown(topBuf);

        /* ---------- BOTTOM SCREEN ---------- */
        C2D_TextBufClear(bottomBuf);
        drawBottomScreen(bottomBuf, bottom);

        C3D_FrameEnd(C3D_FRAME_SYNCDRAW);
    }

    if (settingsDirty) saveSettings();
    records.totalTimeSeconds += sessionFrames / 60;
    saveRecords(); // sempre, perché la riga sopra aggiorna comunque il tempo della sessione corrente
    if (alarmDirty) saveAlarm();

    exitAudio();

    C2D_TextBufDelete(topBuf);
    C2D_TextBufDelete(bottomBuf);
    C2D_TextBufDelete(staticBuf);
    C2D_Fini();
    C3D_Fini();
    mcuHwcExit();
    ptmuExit();
    gfxExit();

    return 0;
}