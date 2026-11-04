#ifndef __TRACK_H
#define __TRACK_H
#include "sys.h"

#define TRACK_EMA_Q   0.35f   /* 每帧平滑系数(4Hz 下 ~0.7s 收敛) */
#define TRACK_PIXELS  768     /* 32x24 */

/* 768 点温度场找最高/最低点输入坐标(0..31, 0..23), 并列取先出现的 */
void Track_Find(const float *to, int *hx, int *hy, int *lx, int *ly);
/* 输出像素坐标一阶指数平滑: s += 0.35*(t - s) */
void Track_Smooth(float *sx, float *sy, float tx, float ty);

#endif
