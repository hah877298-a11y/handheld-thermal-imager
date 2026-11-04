/**
 ******************************************************************************
 * @file    mlx90640.c
 * @brief   MLX90640 32x24 红外阵列驱动
 *
 * 算法移植自 Melexis 官方 MLX90640 API (Apache-2.0):
 *   Copyright (C) 2017 Melexis N.V.
 *   Licensed under the Apache License, Version 2.0
 *   参考源码: ref/mlx90640-official/MLX90640_API.c
 *
 * F103 适配(详见 docs/superpowers/specs/2026-10-02-mlx90640-driver-design.md):
 *   1. double -> float(F103 无 FPU)
 *   2. POW2(pow 函数) -> pow2i 整数幂
 *   3. 提取过程局部大数组 -> 全局共享缓冲 s_tmp[768](栈仅 1KB)
 *   4. C99 -> C90(工程 uC99=0)
 ******************************************************************************
 */
#include "mlx90640.h"
#include "iic.h"
#include <math.h>
#include <stdio.h>   /* DumpFrameToUart 用 printf */

/* ---------------- 全局数据(内存规划见设计文档第 8 节) ---------------- */
uint16_t g_rawBuf[834];     /* 复用: 初始化时存 832 字 EEPROM, 运行时存帧数据 */
paramsMLX90640 g_mlx;       /* 标定参数 */
float g_to[768];            /* 温度输出(°C) */

static float s_tmp[768];    /* 标定提取共享临时缓冲(alpha/kta/kv 三次复用) */

/* ---------------- 工具宏/函数 ---------------- */
#define MS_BYTE(x)    ((uint16_t)((x) >> 8))
#define LS_BYTE(x)    ((uint16_t)((x) & 0xFF))
#define NIB1(x)       ((uint16_t)((x) & 0x000F))
#define NIB2(x)       ((uint16_t)(((x) & 0x00F0) >> 4))
#define NIB3(x)       ((uint16_t)(((x) & 0x0F00) >> 8))
#define NIB4(x)       ((uint16_t)(((x) & 0xF000) >> 12))
#define MSBITS6(x)    ((uint16_t)((x) & 0xFC00))
#define LSBITS10(x)   ((uint16_t)((x) & 0x03FF))
#define SCALE_ALPHA   0.000001f
#define FABS(x)       ((x) < 0.0f ? -(x) : (x))

/* 2 的整数次幂(替代官方 POW2/pow) */
static float pow2i(int n)
{
    float r = 1.0f;
    if(n >= 0)
        while(n-- > 0) r *= 2.0f;
    else
        while(n++ < 0) r *= 0.5f;
    return r;
}

/* 从器件读 nWords 个 16 位字(I2C 大端 -> 小端 CPU 按字交换组装) */
static int MLX_ReadWords(uint8_t dev, uint16_t reg, uint16_t nWords, uint16_t *dst)
{
    uint8_t *buf = (uint8_t *)dst;
    int err;
    uint16_t i;
    err = IIC_ReadBytes(dev, reg, (uint16_t)(nWords << 1), buf);
    if(err != IIC_OK) return err;
    for(i = 0; i < nWords; i++)
        dst[i] = (uint16_t)(((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1]);
    return IIC_OK;
}

/* 总线层错误码 -> 官方 API 负错误码 */
static int MLX_MapIicErr(int err)
{
    if(err == IIC_NACK) return -MLX_ERR_I2C_NACK;
    return -MLX_ERR_I2C_WRITE;
}

/* ==================== 帧/EEPROM 读取 ==================== */

int MLX90640_DumpEE(uint8_t slaveAddr, uint16_t *eeData)
{
    int err;
    err = MLX_ReadWords(slaveAddr, MLX_EEPROM_ADDR, MLX_EEPROM_NUM, eeData);
    if(err != IIC_OK) return MLX_MapIicErr(err);
    return MLX_OK;
}

/* 同步到新帧开始(清就绪标志并等待下一帧就绪) */
int MLX90640_SynchFrame(uint8_t slaveAddr)
{
    uint16_t status = 0;
    int err;
    uint32_t t = 0;

    err = IIC_WriteWord(slaveAddr, MLX_STATUS_REG, MLX_STATUS_INIT);
    if(err != IIC_OK) return MLX_MapIicErr(err);

    do
    {
        err = MLX_ReadWords(slaveAddr, MLX_STATUS_REG, 1, &status);
        if(err != IIC_OK) return MLX_MapIicErr(err);
        if(++t > 200000) return -MLX_ERR_FRAME_DATA;    /* 等不到数据就绪 */
    } while(!(status & MLX_STAT_DATA_READY));

    return MLX_OK;
}

/* 读取一帧: 768 像素 + 64 辅助字 + 控制寄存器 + 子页号(共 834 字) */
int MLX90640_GetFrameData(uint8_t slaveAddr, uint16_t *frameData)
{
    uint16_t status = 0, ctrl = 0, aux[64];
    int err;
    uint32_t t = 0;
    uint16_t i, line;

    /* 1. 等数据就绪 */
    do
    {
        err = MLX_ReadWords(slaveAddr, MLX_STATUS_REG, 1, &status);
        if(err != IIC_OK) return MLX_MapIicErr(err);
        if(++t > 200000) return -MLX_ERR_FRAME_DATA;
    } while(!(status & MLX_STAT_DATA_READY));

    /* 2. 清就绪标志 */
    err = IIC_WriteWord(slaveAddr, MLX_STATUS_REG, MLX_STATUS_INIT);
    if(err != IIC_OK) return MLX_MapIicErr(err);

    /* 3. 读 768 像素 */
    err = MLX_ReadWords(slaveAddr, MLX_RAM_PIX_ADDR, MLX_PIXEL_NUM, frameData);
    if(err != IIC_OK) return MLX_MapIicErr(err);

    /* 4. 读 64 辅助字 */
    err = MLX_ReadWords(slaveAddr, MLX_RAM_AUX_ADDR, MLX_AUX_NUM, aux);
    if(err != IIC_OK) return MLX_MapIicErr(err);

    /* 5. 读控制寄存器 */
    err = MLX_ReadWords(slaveAddr, MLX_CTRL_REG, 1, &ctrl);
    if(err != IIC_OK) return MLX_MapIicErr(err);

    /* 6. 辅助区校验(与官方 ValidateAuxData 相同) */
    if(aux[0] == 0x7FFF) return -MLX_ERR_FRAME_DATA;
    for(i = 8;  i < 19; i++) if(aux[i] == 0x7FFF) return -MLX_ERR_FRAME_DATA;
    for(i = 20; i < 23; i++) if(aux[i] == 0x7FFF) return -MLX_ERR_FRAME_DATA;
    for(i = 24; i < 33; i++) if(aux[i] == 0x7FFF) return -MLX_ERR_FRAME_DATA;
    for(i = 40; i < 51; i++) if(aux[i] == 0x7FFF) return -MLX_ERR_FRAME_DATA;
    for(i = 52; i < 55; i++) if(aux[i] == 0x7FFF) return -MLX_ERR_FRAME_DATA;
    for(i = 56; i < 64; i++) if(aux[i] == 0x7FFF) return -MLX_ERR_FRAME_DATA;

    for(i = 0; i < MLX_AUX_NUM; i++) frameData[FRAME_AUX_BASE + i] = aux[i];
    frameData[FRAME_CTRL_REG] = ctrl;
    frameData[FRAME_SUBPAGE]  = (uint16_t)(status & 0x0001);

    /* 7. 像素校验: 行首 0x7FFF 且行奇偶==子页号 -> 数据无效(同官方) */
    line = 0;
    for(i = 0; i < MLX_PIXEL_NUM; i += MLX_LINE_SIZE)
    {
        if((frameData[i] == 0x7FFF) && (line % 2 == frameData[FRAME_SUBPAGE]))
            return -MLX_ERR_FRAME_DATA;
        line++;
    }

    /* 8. 慢读防护: 读帧期间子页若已切换, 说明跨越了帧边界 -> 拒帧 */
    err = MLX_ReadWords(slaveAddr, MLX_STATUS_REG, 1, &status);
    if(err != IIC_OK) return MLX_MapIicErr(err);
    if((status & 0x0001) != frameData[FRAME_SUBPAGE]) return -MLX_ERR_FRAME_DATA;

    return MLX_OK;
}

/* ==================== 控制寄存器 ==================== */

int MLX90640_SetRefreshRate(uint8_t slaveAddr, uint8_t refreshRate)
{
    uint16_t ctrl = 0;
    int err;

    err = MLX_ReadWords(slaveAddr, MLX_CTRL_REG, 1, &ctrl);
    if(err != IIC_OK) return MLX_MapIicErr(err);

    /* 读-改-写: 清刷新率位段, 写入新值(0..7 -> 0.5Hz..64Hz) */
    ctrl = (ctrl & ~MLX_CTRL_RATE_BITS) | ((uint16_t)(refreshRate & 0x07u) << 7);

    err = IIC_WriteWord(slaveAddr, MLX_CTRL_REG, ctrl);
    if(err != IIC_OK) return MLX_MapIicErr(err);
    return MLX_OK;
}

int MLX90640_GetCurMode(uint8_t slaveAddr)
{
    uint16_t ctrl = 0;
    int err;

    err = MLX_ReadWords(slaveAddr, MLX_CTRL_REG, 1, &ctrl);
    if(err != IIC_OK) return MLX_MapIicErr(err);
    return (int)((ctrl & MLX_CTRL_MODE_BIT) >> 12);    /* 1=棋盘 0=隔行 */
}

/* ==================== 标定参数提取 ==================== */

static void ExtractVDDParameters(uint16_t *ee, paramsMLX90640 *p)
{
    int8_t kvdd;
    int16_t vdd25;

    kvdd = (int8_t)MS_BYTE(ee[51]);
    vdd25 = (int16_t)LS_BYTE(ee[51]);
    vdd25 = (int16_t)(((vdd25 - 256) << 5) - 8192);

    p->kVdd = (int16_t)(32 * kvdd);
    p->vdd25 = vdd25;
}

static void ExtractPTATParameters(uint16_t *ee, paramsMLX90640 *p)
{
    float kvPTAT, ktPTAT;

    kvPTAT = (float)(MSBITS6(ee[50]) >> 10);
    if(kvPTAT > 31.0f) kvPTAT -= 64.0f;
    kvPTAT /= 4096.0f;

    ktPTAT = (float)LSBITS10(ee[50]);
    if(ktPTAT > 511.0f) ktPTAT -= 1024.0f;
    ktPTAT /= 8.0f;

    p->KvPTAT = kvPTAT;
    p->KtPTAT = ktPTAT;
    p->vPTAT25 = ee[49];
    /* 注意: 官方用带位权的掩码值直接除以 2^14, 不是 nibble 值 */
    p->alphaPTAT = (float)(ee[16] & 0xF000) / pow2i(14) + 8.0f;
}

static void ExtractGainParameters(uint16_t *ee, paramsMLX90640 *p)
{
    p->gainEE = (int16_t)ee[48];
}

static void ExtractTgcParameters(uint16_t *ee, paramsMLX90640 *p)
{
    p->tgc = (float)((int8_t)LS_BYTE(ee[60])) / 32.0f;
}

static void ExtractResolutionParameters(uint16_t *ee, paramsMLX90640 *p)
{
    p->resolutionEE = (uint8_t)((ee[56] & 0x3000) >> 12);
}

static void ExtractKsTaParameters(uint16_t *ee, paramsMLX90640 *p)
{
    p->KsTa = (float)((int8_t)MS_BYTE(ee[60])) / 8192.0f;
}

static void ExtractKsToParameters(uint16_t *ee, paramsMLX90640 *p)
{
    int32_t ksToScale;
    int8_t step;

    step = (int8_t)(((ee[63] & 0x3000) >> 12) * 10);

    p->ct[0] = -40;
    p->ct[1] = 0;
    p->ct[2] = (int16_t)((int16_t)NIB2(ee[63]) * step);
    p->ct[3] = (int16_t)(p->ct[2] + (int16_t)((int16_t)NIB3(ee[63]) * step));
    p->ct[4] = 400;

    ksToScale = (int32_t)(NIB1(ee[63]) + 8);
    ksToScale = 1L << ksToScale;

    p->ksTo[0] = (float)((int8_t)LS_BYTE(ee[61])) / (float)ksToScale;
    p->ksTo[1] = (float)((int8_t)MS_BYTE(ee[61])) / (float)ksToScale;
    p->ksTo[2] = (float)((int8_t)LS_BYTE(ee[62])) / (float)ksToScale;
    p->ksTo[3] = (float)((int8_t)MS_BYTE(ee[62])) / (float)ksToScale;
    p->ksTo[4] = -0.0002f;
}

/* 注意: 官方顺序中 CP 参数在 Alpha 之前提取(alpha 归一化要用 cpAlpha) */
static void ExtractCPParameters(uint16_t *ee, paramsMLX90640 *p)
{
    float alphaSP[2];
    int16_t offsetSP[2];
    float cpKv, cpKta;
    uint8_t alphaScale, ktaScale1, kvScale;

    alphaScale = (uint8_t)(NIB4(ee[32]) + 27);

    offsetSP[0] = (int16_t)LSBITS10(ee[58]);
    if(offsetSP[0] > 511) offsetSP[0] -= 1024;
    offsetSP[1] = (int16_t)(MSBITS6(ee[58]) >> 10);
    if(offsetSP[1] > 31) offsetSP[1] -= 64;
    offsetSP[1] = (int16_t)(offsetSP[1] + offsetSP[0]);

    alphaSP[0] = (float)LSBITS10(ee[57]);
    if(alphaSP[0] > 511.0f) alphaSP[0] -= 1024.0f;
    alphaSP[0] /= pow2i(alphaScale);

    alphaSP[1] = (float)(MSBITS6(ee[57]) >> 10);
    if(alphaSP[1] > 31.0f) alphaSP[1] -= 64.0f;
    alphaSP[1] = (1.0f + alphaSP[1] / 128.0f) * alphaSP[0];

    cpKta = (float)((int8_t)LS_BYTE(ee[59]));
    ktaScale1 = (uint8_t)(NIB2(ee[56]) + 8);
    p->cpKta = cpKta / pow2i(ktaScale1);

    cpKv = (float)((int8_t)MS_BYTE(ee[59]));
    kvScale = (uint8_t)NIB3(ee[56]);
    p->cpKv = cpKv / pow2i(kvScale);

    p->cpAlpha[0] = alphaSP[0];
    p->cpAlpha[1] = alphaSP[1];
    p->cpOffset[0] = offsetSP[0];
    p->cpOffset[1] = offsetSP[1];
}

static void ExtractAlphaParameters(uint16_t *ee, paramsMLX90640 *p)
{
    int accRow[24], accColumn[32];
    int i, j, q;
    int alphaRef;
    uint8_t alphaScale;
    uint8_t accRowScale, accColumnScale, accRemScale;
    float temp;

    accRemScale    = (uint8_t)NIB1(ee[32]);
    accColumnScale = (uint8_t)NIB2(ee[32]);
    accRowScale    = (uint8_t)NIB3(ee[32]);
    alphaScale     = (uint8_t)(NIB4(ee[32]) + 30);
    alphaRef       = ee[33];

    for(i = 0; i < 6; i++)
    {
        q = i * 4;
        accRow[q + 0] = NIB1(ee[34 + i]);
        accRow[q + 1] = NIB2(ee[34 + i]);
        accRow[q + 2] = NIB3(ee[34 + i]);
        accRow[q + 3] = NIB4(ee[34 + i]);
    }
    for(i = 0; i < 24; i++) if(accRow[i] > 7) accRow[i] -= 16;

    for(i = 0; i < 8; i++)
    {
        q = i * 4;
        accColumn[q + 0] = NIB1(ee[40 + i]);
        accColumn[q + 1] = NIB2(ee[40 + i]);
        accColumn[q + 2] = NIB3(ee[40 + i]);
        accColumn[q + 3] = NIB4(ee[40 + i]);
    }
    for(i = 0; i < 32; i++) if(accColumn[i] > 7) accColumn[i] -= 16;

    for(i = 0; i < 24; i++)
    {
        for(j = 0; j < 32; j++)
        {
            q = 32 * i + j;
            s_tmp[q] = (float)((ee[64 + q] & 0x03F0) >> 4);
            if(s_tmp[q] > 31.0f) s_tmp[q] -= 64.0f;
            s_tmp[q] *= (float)(1 << accRemScale);
            s_tmp[q] = (float)(alphaRef + (accRow[i] << accRowScale)
                               + (accColumn[j] << accColumnScale)) + s_tmp[q];
            s_tmp[q] /= pow2i(alphaScale);
            s_tmp[q] -= p->tgc * (p->cpAlpha[0] + p->cpAlpha[1]) / 2.0f;
            s_tmp[q] = SCALE_ALPHA / s_tmp[q];
        }
    }

    /* 定点化归一: 放大到 int16 能装下的最大精度, 放大倍数存 alphaScale */
    temp = s_tmp[0];
    for(i = 1; i < 768; i++) if(s_tmp[i] > temp) temp = s_tmp[i];

    alphaScale = 0;
    while(temp < 32767.4f)
    {
        temp *= 2.0f;
        alphaScale++;
    }

    for(i = 0; i < 768; i++)
    {
        temp = s_tmp[i] * pow2i(alphaScale);
        p->alpha[i] = (uint16_t)(temp + 0.5f);
    }
    p->alphaScale = alphaScale;
}

static void ExtractOffsetParameters(uint16_t *ee, paramsMLX90640 *p)
{
    int occRow[24], occColumn[32];
    int i, j, q;
    int16_t offsetRef;
    uint8_t occRowScale, occColumnScale, occRemScale;

    occRemScale    = (uint8_t)NIB1(ee[16]);
    occColumnScale = (uint8_t)NIB2(ee[16]);
    occRowScale    = (uint8_t)NIB3(ee[16]);
    offsetRef      = (int16_t)ee[17];

    for(i = 0; i < 6; i++)
    {
        q = i * 4;
        occRow[q + 0] = NIB1(ee[18 + i]);
        occRow[q + 1] = NIB2(ee[18 + i]);
        occRow[q + 2] = NIB3(ee[18 + i]);
        occRow[q + 3] = NIB4(ee[18 + i]);
    }
    for(i = 0; i < 24; i++) if(occRow[i] > 7) occRow[i] -= 16;

    for(i = 0; i < 8; i++)
    {
        q = i * 4;
        occColumn[q + 0] = NIB1(ee[24 + i]);
        occColumn[q + 1] = NIB2(ee[24 + i]);
        occColumn[q + 2] = NIB3(ee[24 + i]);
        occColumn[q + 3] = NIB4(ee[24 + i]);
    }
    for(i = 0; i < 32; i++) if(occColumn[i] > 7) occColumn[i] -= 16;

    for(i = 0; i < 24; i++)
    {
        for(j = 0; j < 32; j++)
        {
            q = 32 * i + j;
            p->offset[q] = (int16_t)(MSBITS6(ee[64 + q]) >> 10);
            if(p->offset[q] > 31) p->offset[q] -= 64;
            p->offset[q] = (int16_t)(p->offset[q] * (1 << occRemScale));
            p->offset[q] = (int16_t)(offsetRef + (occRow[i] << occRowScale)
                                     + (occColumn[j] << occColumnScale) + p->offset[q]);
        }
    }
}

static void ExtractKtaPixelParameters(uint16_t *ee, paramsMLX90640 *p)
{
    int i, j, q;
    int8_t ktaRC[4];
    uint8_t ktaScale1, ktaScale2, split;
    float temp, a;

    ktaRC[0] = (int8_t)MS_BYTE(ee[54]);
    ktaRC[2] = (int8_t)LS_BYTE(ee[54]);
    ktaRC[1] = (int8_t)MS_BYTE(ee[55]);
    ktaRC[3] = (int8_t)LS_BYTE(ee[55]);

    ktaScale1 = (uint8_t)(NIB2(ee[56]) + 8);
    ktaScale2 = (uint8_t)NIB1(ee[56]);

    for(i = 0; i < 24; i++)
    {
        for(j = 0; j < 32; j++)
        {
            q = 32 * i + j;
            split = (uint8_t)(2 * (q / 32 - (q / 64) * 2) + q % 2);
            s_tmp[q] = (float)((ee[64 + q] & 0x000E) >> 1);
            if(s_tmp[q] > 3.0f) s_tmp[q] -= 8.0f;
            s_tmp[q] *= (float)(1 << ktaScale2);
            s_tmp[q] = (float)ktaRC[split] + s_tmp[q];
            s_tmp[q] /= pow2i(ktaScale1);
        }
    }

    /* 定点化归一: 放大到 int8 装得下的最大精度 */
    temp = FABS(s_tmp[0]);
    for(i = 1; i < 768; i++)
    {
        a = FABS(s_tmp[i]);
        if(a > temp) temp = a;
    }

    ktaScale1 = 0;
    while(temp < 63.4f)
    {
        temp *= 2.0f;
        ktaScale1++;
    }

    for(i = 0; i < 768; i++)
    {
        temp = s_tmp[i] * pow2i(ktaScale1);
        if(temp < 0.0f) p->kta[i] = (int8_t)(temp - 0.5f);
        else            p->kta[i] = (int8_t)(temp + 0.5f);
    }
    p->ktaScale = ktaScale1;
}

static void ExtractKvPixelParameters(uint16_t *ee, paramsMLX90640 *p)
{
    int i, j, q;
    int8_t kvT[4];
    int8_t v;
    uint8_t kvScale, split;
    float temp, a;

    v = (int8_t)NIB4(ee[52]); if(v > 7) v -= 16; kvT[0] = v;
    v = (int8_t)NIB3(ee[52]); if(v > 7) v -= 16; kvT[2] = v;
    v = (int8_t)NIB2(ee[52]); if(v > 7) v -= 16; kvT[1] = v;
    v = (int8_t)NIB1(ee[52]); if(v > 7) v -= 16; kvT[3] = v;

    kvScale = (uint8_t)NIB3(ee[56]);

    for(i = 0; i < 24; i++)
    {
        for(j = 0; j < 32; j++)
        {
            q = 32 * i + j;
            split = (uint8_t)(2 * (q / 32 - (q / 64) * 2) + q % 2);
            s_tmp[q] = (float)kvT[split] / pow2i(kvScale);
        }
    }

    /* 定点化归一: 放大到 int8 装得下的最大精度 */
    temp = FABS(s_tmp[0]);
    for(i = 1; i < 768; i++)
    {
        a = FABS(s_tmp[i]);
        if(a > temp) temp = a;
    }

    kvScale = 0;
    while(temp < 63.4f)
    {
        temp *= 2.0f;
        kvScale++;
    }

    for(i = 0; i < 768; i++)
    {
        temp = s_tmp[i] * pow2i(kvScale);
        if(temp < 0.0f) p->kv[i] = (int8_t)(temp - 0.5f);
        else            p->kv[i] = (int8_t)(temp + 0.5f);
    }
    p->kvScale = kvScale;
}

static void ExtractCILCParameters(uint16_t *ee, paramsMLX90640 *p)
{
    uint8_t calModeEE;
    float c0, c1, c2;

    calModeEE = (uint8_t)(((ee[10] & 0x0800) >> 4) ^ 0x80);

    c0 = (float)(ee[53] & 0x003F);
    if(c0 > 31.0f) c0 -= 64.0f;
    c0 /= 16.0f;

    c1 = (float)((ee[53] & 0x07C0) >> 6);
    if(c1 > 15.0f) c1 -= 32.0f;
    c1 /= 2.0f;

    c2 = (float)((ee[53] & 0xF800) >> 11);
    if(c2 > 15.0f) c2 -= 32.0f;
    c2 /= 8.0f;

    p->calibrationModeEE = calModeEE;
    p->ilChessC[0] = c0;
    p->ilChessC[1] = c1;
    p->ilChessC[2] = c2;
}

static int CheckAdjacentPixels(uint16_t pix1, uint16_t pix2)
{
    int pixPosDif;
    uint16_t lp1 = pix1 >> 5;
    uint16_t lp2 = pix2 >> 5;
    uint16_t cp1 = pix1 - (lp1 << 5);
    uint16_t cp2 = pix2 - (lp2 << 5);

    pixPosDif = (int)(lp1 - lp2);
    if(pixPosDif > -2 && pixPosDif < 2)
    {
        pixPosDif = (int)(cp1 - cp2);
        if(pixPosDif > -2 && pixPosDif < 2)
            return -MLX_ERR_ADJACENT_BAD;
    }
    return MLX_OK;
}

static int ExtractDeviatingPixels(uint16_t *ee, paramsMLX90640 *p)
{
    uint16_t pixCnt = 0;
    uint16_t brokenCnt = 0, outlierCnt = 0;
    int warn = MLX_OK;
    int i;

    for(pixCnt = 0; pixCnt < 5; pixCnt++)
    {
        p->brokenPixels[pixCnt] = 0xFFFF;
        p->outlierPixels[pixCnt] = 0xFFFF;
    }

    pixCnt = 0;
    while(pixCnt < MLX_PIXEL_NUM && brokenCnt < 5 && outlierCnt < 5)
    {
        if(ee[pixCnt + 64] == 0)                       /* 整字为 0: 坏像素 */
        {
            p->brokenPixels[brokenCnt] = pixCnt;
            brokenCnt++;
        }
        else if((ee[pixCnt + 64] & 0x0001) != 0)       /* bit0: 离群像素 */
        {
            p->outlierPixels[outlierCnt] = pixCnt;
            outlierCnt++;
        }
        pixCnt++;
    }

    if(brokenCnt > 4)        warn = -MLX_ERR_BROKEN_PIXELS;
    else if(outlierCnt > 4)  warn = -MLX_ERR_OUTLIER_PIXELS;
    else if(brokenCnt + outlierCnt > 4) warn = -MLX_ERR_BAD_PIXELS;
    else
    {
        /* 相邻双坏像素无法修复 */
        for(pixCnt = 0; pixCnt < brokenCnt; pixCnt++)
            for(i = pixCnt + 1; i < brokenCnt; i++)
                if(CheckAdjacentPixels(p->brokenPixels[pixCnt], p->brokenPixels[i]) != MLX_OK)
                    return -MLX_ERR_ADJACENT_BAD;
        for(pixCnt = 0; pixCnt < outlierCnt; pixCnt++)
            for(i = pixCnt + 1; i < outlierCnt; i++)
                if(CheckAdjacentPixels(p->outlierPixels[pixCnt], p->outlierPixels[i]) != MLX_OK)
                    return -MLX_ERR_ADJACENT_BAD;
        for(pixCnt = 0; pixCnt < brokenCnt; pixCnt++)
            for(i = 0; i < outlierCnt; i++)
                if(CheckAdjacentPixels(p->brokenPixels[pixCnt], p->outlierPixels[i]) != MLX_OK)
                    return -MLX_ERR_ADJACENT_BAD;
    }
    return warn;
}

int MLX90640_ExtractParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    /* 调用顺序与官方一致(Alpha 依赖 CP, 见 ExtractAlphaParameters) */
    ExtractVDDParameters(eeData, mlx90640);
    ExtractPTATParameters(eeData, mlx90640);
    ExtractGainParameters(eeData, mlx90640);
    ExtractTgcParameters(eeData, mlx90640);
    ExtractResolutionParameters(eeData, mlx90640);
    ExtractKsTaParameters(eeData, mlx90640);
    ExtractKsToParameters(eeData, mlx90640);
    ExtractCPParameters(eeData, mlx90640);
    ExtractAlphaParameters(eeData, mlx90640);
    ExtractOffsetParameters(eeData, mlx90640);
    ExtractKtaPixelParameters(eeData, mlx90640);
    ExtractKvPixelParameters(eeData, mlx90640);
    ExtractCILCParameters(eeData, mlx90640);
    return ExtractDeviatingPixels(eeData, mlx90640);
}

/* ==================== 温度解算 ==================== */

float MLX90640_GetVdd(uint16_t *frameData, const paramsMLX90640 *params)
{
    float vdd;
    float resolutionCorrection;
    uint16_t resolutionRAM;

    resolutionRAM = (uint16_t)((frameData[FRAME_CTRL_REG] & 0x0C00) >> 10);
    resolutionCorrection = pow2i(params->resolutionEE) / pow2i((int)resolutionRAM);
    vdd = (resolutionCorrection * (float)(int16_t)frameData[FRAME_AUX_VDD]
           - (float)params->vdd25) / (float)params->kVdd + 3.3f;
    return vdd;
}

float MLX90640_GetTa(uint16_t *frameData, const paramsMLX90640 *params)
{
    int16_t ptat;
    float ptatArt, vdd, ta;

    vdd = MLX90640_GetVdd(frameData, params);
    ptat = (int16_t)frameData[FRAME_AUX_PTAT];
    ptatArt = ((float)ptat / ((float)ptat * params->alphaPTAT
               + (float)(int16_t)frameData[FRAME_AUX_BASE])) * pow2i(18);
    ta = (ptatArt / (1.0f + params->KvPTAT * (vdd - 3.3f))
          - (float)params->vPTAT25);
    ta = ta / params->KtPTAT + 25.0f;
    return ta;
}

/* 核心算法: 768 像素辐照度补偿 + 普朗克反演求温度(详见设计文档第 4 节) */
void MLX90640_CalculateTo(uint16_t *frameData, const paramsMLX90640 *params,
                          float emissivity, float tr, float *result)
{
    float vdd, ta, ta4, tr4, taTr;
    float gain;
    float irDataCP[2];
    float irData, alphaCompensated, Sx, To;
    float alphaCorrR[4];
    float ktaScale, kvScale, alphaScale;
    float kta, kv;
    uint8_t mode;
    int8_t ilPattern, chessPattern, pattern, conversionPattern, range;
    uint16_t subPage;
    int pixelNumber;

    subPage = frameData[FRAME_SUBPAGE];
    vdd = MLX90640_GetVdd(frameData, params);
    ta  = MLX90640_GetTa(frameData, params);

    /* 斯特藩-玻尔兹曼环境项: 总辐照 = ε·目标 + (1-ε)·环境反射 */
    ta4 = ta + 273.15f;
    ta4 = ta4 * ta4;
    ta4 = ta4 * ta4;
    tr4 = tr + 273.15f;
    tr4 = tr4 * tr4;
    tr4 = tr4 * tr4;
    taTr = tr4 - (tr4 - ta4) / emissivity;

    ktaScale   = pow2i(params->ktaScale);
    kvScale    = pow2i(params->kvScale);
    alphaScale = pow2i(params->alphaScale);

    /* 分段灵敏度修正 */
    alphaCorrR[0] = 1.0f / (1.0f + params->ksTo[0] * 40.0f);
    alphaCorrR[1] = 1.0f;
    alphaCorrR[2] = (1.0f + params->ksTo[1] * (float)params->ct[2]);
    alphaCorrR[3] = alphaCorrR[2] * (1.0f + params->ksTo[2]
                    * (float)(params->ct[3] - params->ct[2]));

    /* 增益逐帧归一 */
    gain = (float)params->gainEE / (float)(int16_t)frameData[FRAME_AUX_GAIN];

    /* 模式: 官方按 bit12>>5 得到 0x00/0x80, 与 calibrationModeEE 对齐 */
    mode = (uint8_t)((frameData[FRAME_CTRL_REG] & MLX_CTRL_MODE_BIT) >> 5);

    /* 两个补偿像素 */
    irDataCP[0] = (float)(int16_t)frameData[FRAME_AUX_CP0] * gain;
    irDataCP[1] = (float)(int16_t)frameData[FRAME_AUX_CP1] * gain;

    irDataCP[0] -= (float)params->cpOffset[0] * (1.0f + params->cpKta * (ta - 25.0f))
                   * (1.0f + params->cpKv * (vdd - 3.3f));
    if(mode == params->calibrationModeEE)
    {
        irDataCP[1] -= (float)params->cpOffset[1] * (1.0f + params->cpKta * (ta - 25.0f))
                       * (1.0f + params->cpKv * (vdd - 3.3f));
    }
    else
    {
        irDataCP[1] -= ((float)params->cpOffset[1] + params->ilChessC[0])
                       * (1.0f + params->cpKta * (ta - 25.0f))
                       * (1.0f + params->cpKv * (vdd - 3.3f));
    }

    for(pixelNumber = 0; pixelNumber < 768; pixelNumber++)
    {
        ilPattern = (int8_t)(pixelNumber / 32 - (pixelNumber / 64) * 2);
        chessPattern = (int8_t)(ilPattern ^ (pixelNumber - (pixelNumber / 2) * 2));
        conversionPattern = (int8_t)(((pixelNumber + 2) / 4 - (pixelNumber + 3) / 4
                             + (pixelNumber + 1) / 4 - pixelNumber / 4)
                             * (1 - 2 * ilPattern));

        if(mode == 0) pattern = ilPattern;
        else          pattern = chessPattern;

        if(pattern == (int8_t)subPage)
        {
            irData = (float)(int16_t)frameData[pixelNumber] * gain;

            kta = (float)params->kta[pixelNumber] / ktaScale;
            kv  = (float)params->kv[pixelNumber] / kvScale;
            irData -= (float)params->offset[pixelNumber]
                      * (1.0f + kta * (ta - 25.0f)) * (1.0f + kv * (vdd - 3.3f));

            if(mode != params->calibrationModeEE)
            {
                irData += params->ilChessC[2] * (float)(2 * ilPattern - 1)
                          - params->ilChessC[1] * (float)conversionPattern;
            }

            irData -= params->tgc * irDataCP[subPage];
            irData /= emissivity;

            alphaCompensated = SCALE_ALPHA * alphaScale / (float)params->alpha[pixelNumber];
            alphaCompensated *= (1.0f + params->KsTa * (ta - 25.0f));

            Sx = alphaCompensated * alphaCompensated * alphaCompensated
                 * (irData + alphaCompensated * taTr);
            Sx = sqrtf(sqrtf(Sx)) * params->ksTo[1];

            /* 一阶近似(√√ = 四次根, 辐射能量 ∝ T^4) */
            To = sqrtf(sqrtf(irData / (alphaCompensated
                 * (1.0f - params->ksTo[1] * 273.15f) + Sx) + taTr)) - 273.15f;

            /* 按温度区间取分段系数, 二次迭代精修 */
            if(To < (float)params->ct[1])       range = 0;
            else if(To < (float)params->ct[2])  range = 1;
            else if(To < (float)params->ct[3])  range = 2;
            else                                range = 3;

            To = sqrtf(sqrtf(irData / (alphaCompensated * alphaCorrR[range]
                 * (1.0f + params->ksTo[range] * (To - (float)params->ct[range])))
                 + taTr)) - 273.15f;

            result[pixelNumber] = To;
        }
    }
}

/* ==================== 坏像素修复 ==================== */

static float GetMedian(float *values, int n)
{
    float temp;
    int i, j;

    for(i = 0; i < n - 1; i++)
    {
        for(j = i + 1; j < n; j++)
        {
            if(values[j] < values[i])
            {
                temp = values[i];
                values[i] = values[j];
                values[j] = temp;
            }
        }
    }
    if(n % 2 == 0) return (values[n / 2] + values[n / 2 - 1]) / 2.0f;
    return values[n / 2];
}

static int IsPixelBad(uint16_t pixel, paramsMLX90640 *params)
{
    int i;
    for(i = 0; i < 5; i++)
    {
        if(pixel == params->outlierPixels[i] || pixel == params->brokenPixels[i])
            return 1;
    }
    return 0;
}

/* mode=1: 相邻像素中值(隔行模式); mode=0: 行方向外推(棋盘模式) */
void MLX90640_BadPixelsCorrection(uint16_t *pixels, float *to, int mode,
                                  paramsMLX90640 *params)
{
    float ap[4];
    uint8_t pix = 0;

    while(pixels[pix] != 0xFFFF && pix < 5)
    {
        uint16_t pidx = pixels[pix];
        uint8_t line = (uint8_t)(pidx >> 5);
        uint8_t column = (uint8_t)(pidx - ((uint16_t)line << 5));

        if(mode == 1)
        {
            if(line == 0)
            {
                if(column == 0)       to[pidx] = to[33];
                else if(column == 31) to[pidx] = to[62];
                else to[pidx] = (to[pidx + 31] + to[pidx + 33]) / 2.0f;
            }
            else if(line == 23)
            {
                if(column == 0)       to[pidx] = to[705];
                else if(column == 31) to[pidx] = to[734];
                else to[pidx] = (to[pidx - 33] + to[pidx - 31]) / 2.0f;
            }
            else if(column == 0)
            {
                to[pidx] = (to[pidx - 31] + to[pidx + 33]) / 2.0f;
            }
            else if(column == 31)
            {
                to[pidx] = (to[pidx - 33] + to[pidx + 31]) / 2.0f;
            }
            else
            {
                ap[0] = to[pidx - 33];
                ap[1] = to[pidx - 31];
                ap[2] = to[pidx + 31];
                ap[3] = to[pidx + 33];
                to[pidx] = GetMedian(ap, 4);
            }
        }
        else
        {
            if(column == 0)        to[pidx] = to[pidx + 1];
            else if(column == 1 || column == 30)
                to[pidx] = (to[pidx - 1] + to[pidx + 1]) / 2.0f;
            else if(column == 31)  to[pidx] = to[pidx - 1];
            else
            {
                if(!IsPixelBad(pidx - 2, params) && !IsPixelBad(pidx + 2, params))
                {
                    ap[0] = to[pidx + 1] - to[pidx + 2];
                    ap[1] = to[pidx - 1] - to[pidx - 2];
                    if(FABS(ap[0]) > FABS(ap[1]))
                        to[pidx] = to[pidx - 1] + ap[1];
                    else
                        to[pidx] = to[pidx + 1] + ap[0];
                }
                else
                {
                    to[pidx] = (to[pidx - 1] + to[pidx + 1]) / 2.0f;
                }
            }
        }
        pix++;
    }
}

/* ==================== 应用接口(本设计新增) ==================== */

int MLX90640_Init(void)
{
    int err;
    uint16_t *ee = g_rawBuf;      /* 前 832 字用作 EEPROM 缓冲 */

    err = MLX90640_DumpEE(MLX90640_ADDR, ee);
    if(err != MLX_OK) return err;

    /* 器件校验: EEPROM 0x240F 低字节 = I2C 地址(数据手册 Table 8, 示例 BE33)
     * 注: 0x240F 不是设备 ID, ID1~3 在 0x2407~0x2409(Table 7) */
    if((ee[15] & 0xFF) != MLX90640_ADDR) return -MLX_ERR_NO_DEVICE;

    err = MLX90640_ExtractParameters(ee, &g_mlx);
    if(err != MLX_OK) return err;

    err = MLX90640_SynchFrame(MLX90640_ADDR);
    if(err != MLX_OK) return err;

    err = MLX90640_SetRefreshRate(MLX90640_ADDR, 3);   /* 4Hz: bit-bang 读帧~200ms < 250ms 帧周期, 避免撕裂拒帧(-8) */
    if(err != MLX_OK) return err;

    return MLX_OK;
}

void MLX90640_ToFixed(int16_t *dst, const float *to)
{
    int i;
    for(i = 0; i < 768; i++)
        dst[i] = (int16_t)(to[i] * 10.0f + (to[i] >= 0.0f ? 0.5f : -0.5f));
}

void MLX90640_DumpFrameToUart(const float *to)
{
    int i;
    printf("--- frame dump (degC, 24x32) ---\r\n");
    for(i = 0; i < 768; i++)
    {
        printf("%6.1f", (double)to[i]);
        if(i % 32 == 31) printf("\r\n");
    }
}
