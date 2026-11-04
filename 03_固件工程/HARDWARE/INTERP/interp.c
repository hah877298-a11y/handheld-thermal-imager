#include "interp.h"

static u8 s_w[8], s_iw[8];    /* 预计算权重: w[u]=u, iw[u]=8-u */

void Interp_Init(void)
{
    u8 i;
    for(i = 0; i < 8; i++) { s_w[i] = i; s_iw[i] = (u8)(8 - i); }
}

void Interp_RenderStrip(u16 y, const u8 *idx, u8 mode, u8 *idxStrip)
{
    u16 row = (u16)((y >> 3) * INTERP_IN_COLS);      /* 本条带对应输入行 */
    u16 r1row = ((y >> 3) + 1 < INTERP_IN_ROWS) ? (u16)(row + INTERP_IN_COLS) : row;
    const u8 *r0 = idx + row;
    const u8 *r1 = idx + r1row;
    u16 x, line;

    if(mode == INTERP_MODE_NEAREST)
    {
        for(line = 0; line < INTERP_STRIP_H; line++)
        {
            const u8 *src = (line < 4) ? r0 : r1;    /* 最近行 */
            u8 *dst = idxStrip + line * INTERP_OUT_COLS;
            for(x = 0; x < INTERP_OUT_COLS; x++)
            {
                u16 c = (u16)((x + 4) >> 3);         /* 最近列 */
                if(c > INTERP_IN_COLS - 1) c = INTERP_IN_COLS - 1;
                dst[x] = src[c];
            }
        }
    }
    else
    {
        u8 top[INTERP_OUT_COLS], bot[INTERP_OUT_COLS];

        /* 水平插值: 与输出行号无关, 8 条输出行共用一次(计算量降 8 倍的关键) */
        {
            u16 c = 0;
            for(x = 0; x < INTERP_OUT_COLS; x++)
            {
                u8  u   = (u8)(x & 7);
                u16 c1  = (c < INTERP_IN_COLS - 1) ? (u16)(c + 1) : c;
                top[x] = (u8)((r0[c] * s_iw[u] + r0[c1] * s_w[u] + 4) >> 3);
                bot[x] = (u8)((r1[c] * s_iw[u] + r1[c1] * s_w[u] + 4) >> 3);
                if(u == 7 && c < INTERP_IN_COLS - 1) c++;
            }
        }

        /* 垂直插值: 每输出行一次(指针化热循环) */
        {
            u8 *dst = idxStrip;
            for(line = 0; line < INTERP_STRIP_H; line++)
            {
                u8  rw0 = s_iw[line], rw1 = s_w[line];
                const u8 *pt = top, *pb = bot;
                u16 px;
                for(px = 0; px < INTERP_OUT_COLS; px++)
                    *dst++ = (u8)((*pt++ * rw0 + *pb++ * rw1 + 4) >> 3);
            }
        }
    }
}
