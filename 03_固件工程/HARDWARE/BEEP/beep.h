#ifndef __BEEP_H
#define __BEEP_H
#include "sys.h"

#define BEEP PBout(8)     /* 蜂鸣器 PB8: 1=响 0=停 */

void BEEP_Init(void);

#endif
