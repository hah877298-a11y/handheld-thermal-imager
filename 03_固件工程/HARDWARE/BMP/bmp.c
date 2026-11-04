#include "bmp.h"

/* 24bpp BMP: 14 字节文件头 + 40 字节信息头 + 自底向上像素行 */
void Bmp_WriteHeader(FIL *f, u16 w, u16 h)
{
    u8 hdr[54];
    u32 fsize = 54u + (u32)w * (u32)h * 3u;
    u32 isize = (u32)w * (u32)h * 3u;
    u16 i;
    UINT bw;
    for(i = 0; i < 54; i++) hdr[i] = 0;
    hdr[0]  = 'B'; hdr[1] = 'M';
    hdr[2]  = (u8)(fsize); hdr[3] = (u8)(fsize >> 8);
    hdr[4]  = (u8)(fsize >> 16); hdr[5] = (u8)(fsize >> 24);
    hdr[10] = 54;              /* 像素数据偏移 */
    hdr[14] = 40;              /* BITMAPINFOHEADER 大小 */
    hdr[18] = (u8)(w); hdr[19] = (u8)(w >> 8);
    hdr[22] = (u8)(h); hdr[23] = (u8)(h >> 8);   /* 正高度 = 自底向上 */
    hdr[26] = 1;               /* 平面数 */
    hdr[28] = 24;              /* 24bpp */
    hdr[34] = (u8)(isize); hdr[35] = (u8)(isize >> 8);
    hdr[36] = (u8)(isize >> 16); hdr[37] = (u8)(isize >> 24);
    f_write(f, hdr, 54, &bw);
}

void Bmp_WriteRow(FIL *f, const u8 *rgb888, u16 w)
{
    UINT bw;
    f_write(f, rgb888, (UINT)w * 3u, &bw);
}
