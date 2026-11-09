#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define BIT(n) (1u<<(n))
#define RGB15(r,g,b) ((r)|((g)<<5)|((b)<<10))
enum { KEY_A=1, KEY_B=2, KEY_SELECT=4, KEY_START=8, KEY_RIGHT=16, KEY_LEFT=32, KEY_UP=64, KEY_DOWN=128, KEY_R=256, KEY_L=512, KEY_X=1024, KEY_Y=2048, KEY_TOUCH=4096 };
typedef struct { int px, py; } touchPosition;
enum { MODE_0_2D, MODE_5_2D }; enum { VRAM_A_MAIN_BG, VRAM_B_LCD, VRAM_C_SUB_BG, VRAM_D_LCD };
enum { BgType_Bmp16 }; enum { BgSize_B16_256x256 };
void videoSetMode(int); void videoSetModeSub(int);
void vramSetPrimaryBanks(int,int,int,int); void vramSetBankA(int);
int bgInit(int,int,int,int,int); int bgInitSub(int,int,int,int,int); u16 *bgGetGfxPtr(int);
void consoleDemoInit(void); void consoleClear(void);
#define iprintf printf
void scanKeys(void); u32 keysDown(void); u32 keysHeld(void); void touchRead(touchPosition*);
void swiWaitForVBlank(void); int isDSiMode(void);
typedef struct { int dummy; } PrintConsole;
enum { BgType_Text4bpp = 1 }; enum { BgSize_T_256x256 = 1 };
void consoleInit(PrintConsole*,int,int,int,int,int,int,int); void consoleSelect(PrintConsole*); void vramSetBankC(int);
typedef int16_t s16;
