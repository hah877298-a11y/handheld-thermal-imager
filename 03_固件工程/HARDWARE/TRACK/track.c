#include "track.h"

void Track_Find(const float *to, int *hx, int *hy, int *lx, int *ly)
{
    float hmax = to[0], lmin = to[0];
    int i;
    *hx = 0; *hy = 0; *lx = 0; *ly = 0;
    for(i = 1; i < TRACK_PIXELS; i++)
    {
        if(to[i] > hmax) { hmax = to[i]; *hx = i % 32; *hy = i / 32; }
        if(to[i] < lmin) { lmin = to[i]; *lx = i % 32; *ly = i / 32; }
    }
}

void Track_Smooth(float *sx, float *sy, float tx, float ty)
{
    *sx += TRACK_EMA_Q * (tx - *sx);
    *sy += TRACK_EMA_Q * (ty - *sy);
}
