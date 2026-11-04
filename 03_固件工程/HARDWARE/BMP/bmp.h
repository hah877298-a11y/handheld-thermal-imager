#ifndef __BMP_H
#define __BMP_H
#include "sys.h"
#include "ff.h"

void Bmp_WriteHeader(FIL *f, u16 w, u16 h);         /* 54 字节 24bpp 头 */
void Bmp_WriteRow(FIL *f, const u8 *rgb888, u16 w); /* 一行 RGB888, w*3 字节 */

#endif
