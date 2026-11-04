#ifndef __PALETTE_H
#define __PALETTE_H
#include "sys.h"

#define PALETTE_LEVELS  256
#define PALETTE_STRIP_N 2048        /* 256 列 x 8 行条带 */

typedef enum { PAL_IRON = 0, PAL_RAINBOW, PAL_GRAY, PAL_NUM } PaletteId;

extern const u16 g_palettes[PAL_NUM][PALETTE_LEVELS];   /* 三调色板 LUT */

void Palette_Select(PaletteId id);          /* 切换活动调色板, 下一帧生效 */
u8   Palette_Current(void);                 /* 当前调色板 id */
u16  Palette_LookupColor(u8 idx);           /* 色阶 -> RGB565(查活动表) */
void Palette_ColorizeStrip(const u8 *idxStrip, u16 *rgbStrip);/* 2048 点条带查色(活动表) */
/* 768 点温度 -> 色阶 0..255; tmin/tmax 为映射范围;
 * 均匀场(tmax-tmin<0.5°C)全部映射 128 防除零; 越界钳制 */
void Palette_MapIdx(const float *to, float tmin, float tmax, u8 *idx);

#endif
