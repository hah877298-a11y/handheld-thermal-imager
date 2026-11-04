#include "palette.h"

const u16 g_palettes[PAL_NUM][PALETTE_LEVELS] = {
    {
#include "palette_ironbow.inc"
    },
    {
#include "palette_rainbow.inc"
    },
    {
#include "palette_gray.inc"
    },
};

static u8 s_active = PAL_IRON;

void Palette_Select(PaletteId id)
{
    s_active = (u8)id;
}

u8 Palette_Current(void)
{
    return s_active;
}

u16 Palette_LookupColor(u8 idx)
{
    return g_palettes[s_active][idx];
}

void Palette_ColorizeStrip(const u8 *idxStrip, u16 *rgbStrip)
{
    u16 i;
    const u8 *p = idxStrip;
    u16 *q = rgbStrip;
    const u16 *lut = g_palettes[s_active];
    for(i = 0; i < PALETTE_STRIP_N; i++)
        *q++ = lut[*p++];
}

void Palette_MapIdx(const float *to, float tmin, float tmax, u8 *idx)
{
    float range = tmax - tmin;
    u32 scale8;
    u16 i;

    if(range < 0.5f)                       /* 均匀场: 全映射中间色阶 */
    {
        for(i = 0; i < 768; i++) idx[i] = 128;
        return;
    }
    scale8 = (u32)(255.0f * 256.0f / (range * 10.0f) + 0.5f);   /* 每帧一次(四舍五入防顶端欠射) */

    for(i = 0; i < 768; i++)
    {
        int v = (int)((to[i] - tmin) * 10.0f);   /* 0.1°C 定点 */
        int s;
        if(v < 0) v = 0;
        s = (int)(((u32)v * scale8) >> 8);
        if(s > 255) s = 255;
        idx[i] = (u8)s;
    }
}
