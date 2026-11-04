#ifndef __IIC_H
#define __IIC_H
#include "sys.h"

/* 总线错误码 */
#define IIC_OK       0    /* 成功 */
#define IIC_NACK     1    /* 设备无应答（未接/接反/地址错） */
#define IIC_TIMEOUT  2    /* 总线超时 */

void IIC_Init(void);                                      /* I2C1 PB6/PB7 400kHz */
u8 IIC_ReadBytes(u8 dev, u16 reg, u16 n, u8 *buf);        /* 读 n 字节，字节序大端 */
u8 IIC_WriteWord(u8 dev, u16 reg, u16 data);              /* 写一个 16 位字 */

#endif
