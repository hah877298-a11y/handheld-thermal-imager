#ifndef __MLX90640_H
#define __MLX90640_H
#include <stdint.h>

/************************************************
 MLX90640 32x24 红外阵列驱动接口
 移植自 Melexis 官方 MLX90640 API (Apache-2.0)
 结构体/函数名与官方库一一对应, 便于论文对照
************************************************/

/* 器件 */
#define MLX90640_ADDR       0x33u   /* 7 位 I2C 地址 */

/* 地址映射 */
#define MLX_EEPROM_ADDR     0x2400u /* 832 字标定数据 */
#define MLX_EEPROM_NUM      832u
#define MLX_RAM_PIX_ADDR    0x0400u /* 768 像素 */
#define MLX_PIXEL_NUM       768u
#define MLX_LINE_NUM        24u
#define MLX_LINE_SIZE       32u
#define MLX_RAM_AUX_ADDR    0x0700u /* 64 辅助字(PTAT/增益/Vdd/CP) */
#define MLX_AUX_NUM         64u
#define MLX_STATUS_REG      0x8000u /* bit0 子页号, bit3 数据就绪 */
#define MLX_STATUS_INIT     0x0030u /* 清数据就绪的写入值 */
#define MLX_STAT_DATA_READY 0x0008u
#define MLX_CTRL_REG        0x800Du /* 刷新率/分辨率/测量模式 */

/* 控制寄存器位段 */
#define MLX_CTRL_RATE_BITS  (0x7u << 7)    /* 刷新率: 0=0.5Hz..4=8Hz..7=64Hz */
#define MLX_CTRL_MODE_BIT   (0x1u << 12)   /* 1=棋盘模式 0=隔行模式 */

/* 错误码(官方语义: 函数返回负值) */
#define MLX_OK                  0
#define MLX_ERR_I2C_NACK        1
#define MLX_ERR_I2C_WRITE       2
#define MLX_ERR_BROKEN_PIXELS   3
#define MLX_ERR_OUTLIER_PIXELS  4
#define MLX_ERR_BAD_PIXELS      5
#define MLX_ERR_ADJACENT_BAD    6
#define MLX_ERR_EEPROM_DATA     7
#define MLX_ERR_FRAME_DATA      8
#define MLX_ERR_NO_DEVICE       9   /* 本设计新增: 设备 ID 校验失败 */

/* 帧缓冲布局(GetFrameData 输出, 共 834 字) */
#define FRAME_AUX_BASE   768u    /* 辅助区起始 */
#define FRAME_AUX_CP0    776u    /* 补偿像素 0 */
#define FRAME_AUX_GAIN   778u    /* 增益参考像素 */
#define FRAME_AUX_PTAT   800u    /* PTAT 值 */
#define FRAME_AUX_CP1    808u    /* 补偿像素 1 */
#define FRAME_AUX_VDD    810u    /* Vdd 参考像素 */
#define FRAME_CTRL_REG   832u    /* 控制寄存器快照 */
#define FRAME_SUBPAGE    833u    /* 子页号 */

/* 观测参数(可调, 论文可讲) */
#define MLX_EMISSIVITY   0.95f   /* 目标发射率 */
#define MLX_TREFL        20.0f   /* 反射环境温度 °C */

/* 标定参数结构体(与 Melexis 官方库同名同构, 字段顺序一致) */
typedef struct
{
    int16_t  kVdd;
    int16_t  vdd25;
    float    KvPTAT;
    float    KtPTAT;
    uint16_t vPTAT25;
    float    alphaPTAT;
    int16_t  gainEE;
    float    tgc;
    float    cpKv;
    float    cpKta;
    uint8_t  resolutionEE;
    uint8_t  calibrationModeEE;
    float    KsTa;
    float    ksTo[5];
    int16_t  ct[5];
    uint16_t alpha[768];
    uint8_t  alphaScale;
    int16_t  offset[768];
    int8_t   kta[768];
    uint8_t  ktaScale;
    int8_t   kv[768];
    uint8_t  kvScale;
    float    cpAlpha[2];
    int16_t  cpOffset[2];
    float    ilChessC[3];
    uint16_t brokenPixels[5];
    uint16_t outlierPixels[5];
} paramsMLX90640;

/* 与官方库同名的 API */
int  MLX90640_DumpEE(uint8_t slaveAddr, uint16_t *eeData);
int  MLX90640_SynchFrame(uint8_t slaveAddr);
int  MLX90640_GetFrameData(uint8_t slaveAddr, uint16_t *frameData);
int  MLX90640_ExtractParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
float MLX90640_GetVdd(uint16_t *frameData, const paramsMLX90640 *params);
float MLX90640_GetTa(uint16_t *frameData, const paramsMLX90640 *params);
void MLX90640_CalculateTo(uint16_t *frameData, const paramsMLX90640 *params,
                          float emissivity, float tr, float *result);
int  MLX90640_SetRefreshRate(uint8_t slaveAddr, uint8_t refreshRate);
int  MLX90640_GetCurMode(uint8_t slaveAddr);
void MLX90640_BadPixelsCorrection(uint16_t *pixels, float *to, int mode,
                                  paramsMLX90640 *params);

/* 本设计新增接口 */
int  MLX90640_Init(void);                              /* 读EEPROM+ID校验+提取+同步+8Hz */
void MLX90640_ToFixed(int16_t *dst, const float *to);  /* °C -> x10 定点(功能 2 接口) */
void MLX90640_DumpFrameToUart(const float *to);        /* 整帧打印(联调) */

/* 全局数据(内存规划见设计文档第 8 节) */
extern uint16_t g_rawBuf[834];     /* 复用: 初始化存 EEPROM, 运行时存帧 */
extern paramsMLX90640 g_mlx;       /* 标定参数 */
extern float g_to[768];            /* 温度输出 °C */

#endif
