#ifndef __INTERP_H
#define __INTERP_H
#include "sys.h"

#define INTERP_MODE_NEAREST   0   /* 最近邻(对比组) */
#define INTERP_MODE_BILINEAR  1   /* 定点 Q8 双线性(实验组) */

#define INTERP_IN_COLS   32
#define INTERP_IN_ROWS   24
#define INTERP_SCALE     8
#define INTERP_OUT_COLS  (INTERP_IN_COLS * INTERP_SCALE)   /* 256 */
#define INTERP_OUT_ROWS  (INTERP_IN_ROWS * INTERP_SCALE)   /* 192 */
#define INTERP_STRIP_H   8                                 /* 条带高 8 行 */
#define INTERP_STRIP_N   (INTERP_OUT_COLS * INTERP_STRIP_H)/* 2048 */

void Interp_Init(void);   /* 预计算权重表 s_w[8]={0..7} s_iw[8]={8..1} */
/* 渲染一条条带: y 为条带顶行(0,8,...,184), idx 为 32x24 色阶场(行优先),
 * mode 为插值模式, 输出 idxStrip[2048] 色阶 */
void Interp_RenderStrip(u16 y, const u8 *idx, u8 mode, u8 *idxStrip);

#endif
