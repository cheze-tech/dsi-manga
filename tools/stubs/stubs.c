#include "nds.h"
#include "calico/nds/pm.h"
#include "tjpgd.h"
#include "fat.h"
#ifndef STUBS_REAL_TJPGD
JRESULT jd_prepare(JDEC*a, size_t(*b)(JDEC*,uint8_t*,size_t), void*c, size_t d, void*e){return JDR_FMT3;}
JRESULT jd_decomp(JDEC*a, int(*b)(JDEC*,void*,JRECT*), uint8_t c){return JDR_FMT3;}
JRESULT jd_decomp_begin(JDEC*a, int(*b)(JDEC*,void*,JRECT*), uint8_t c){return JDR_FMT3;}
JRESULT jd_decomp_step(JDEC*a, int(*b)(JDEC*,void*,JRECT*), unsigned int d){return JDR_FMT3;}
#endif
void scanKeys(void){} u32 keysDown(void){return 0;} u32 keysHeld(void){return 0;} void touchRead(touchPosition*t){}
void consoleDemoInit(void){} void consoleClear(void){} int fatInitDefault(void){return 1;}
void swiWaitForVBlank(void){} void videoSetMode(int a){} void videoSetModeSub(int a){}
void vramSetPrimaryBanks(int a,int b,int c,int d){} void vramSetBankA(int a){}
int bgInit(int a,int b,int c,int d,int e){return 0;} int bgInitSub(int a,int b,int c,int d,int e){return 1;} u16 *bgGetGfxPtr(int i){return 0;}
int isDSiMode(void){return 1;}
void consoleInit(PrintConsole*a,int b,int c,int d,int e,int f,int g,int h){} void consoleSelect(PrintConsole*a){} void vramSetBankC(int a){}
void audio_init(void){} void audio_pump(int n){(void)n;} void audio_next(void){} void audio_toggle(void){}
int audio_paused(void){return 0;}
int audio_select(int i) { (void)i; return 0; }

/* Test hooks: keep the render harness independent of a real MP3 library. */
static const char *stub_track_name, *stub_track_dir = "/mp3/";
static unsigned stub_track_serial;
void test_audio_set_track(const char *dir, const char *name) {
    stub_track_dir = dir ? dir : "/mp3/";
    stub_track_name = name;
    stub_track_serial++;
}
const char *audio_title(void) { return stub_track_name; }

/* power management / music metadata stubs used by the host render harness */
bool pmMainLoop(void) { return true; }
bool pmIsSleepAllowed(void) { return true; }
void pmSetSleepAllowed(bool allowed) { (void)allowed; }
void pmAddEventHandler(PmEventCookie *cookie, PmEventFn handler, void *user) { (void)cookie; (void)handler; (void)user; }
void pmRemoveEventHandler(PmEventCookie *cookie) { (void)cookie; }
void pmEnterSleep(void) {}
const char *audio_name(int i) { return i == 0 ? stub_track_name : NULL; }
int audio_count(void) { return stub_track_name ? 1 : 0; }
int audio_current_index(void) { return stub_track_name ? 0 : -1; }
int audio_next_index(void) { return -1; }
const char *audio_next_title(void) { return NULL; }
unsigned audio_track_serial(void) { return stub_track_serial; }
const char *audio_music_dir(void) { return stub_track_dir; }
const unsigned char default_font_bin[128 * 8] = { 0 };
