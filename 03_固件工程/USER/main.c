#include "led.h"
#include "delay.h"
#include "sys.h"
#include "lcd.h"
#include "usart.h"
#include "iic.h"
#include "mlx90640.h"
#include "key.h"
#include "interp.h"
#include "palette.h"
#include "track.h"
#include "beep.h"
#include "sdio_sdcard.h"
#include "ff.h"
#include "bmp.h"

/* DWT 周期计数器直访(FWLIB 3.5 core_cm3.h 无 DWT 定义), 用于渲染耗时统计 */
#define DEMCR        (*(volatile u32 *)0xE000EDFCu)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL     (*(volatile u32 *)0xE0001000u)
#define DWT_CYCCNT   (*(volatile u32 *)0xE0001004u)

/************************************************
 手持热成像仪 - 功能 1+2: MLX90640 温度采集 + 双线性插值热力图
 帧循环: 读帧 -> 解算 -> 坏点修正 -> 色阶映射 -> 条带插值渲染
         -> 统计 -> 1Hz 打印 + LCD 刷新
************************************************/

static int16_t s_toFixed[768];   /* 缩放温度(x10, 供后续功能使用) */
static u8  s_idx[768];           /* 32x24 色阶场 */
static u8  s_idxStrip[2048];     /* 条带缓冲(色阶) */
static u16 s_rgbStrip[2048];     /* 条带缓冲(RGB565) */
static u8  s_mode = INTERP_MODE_BILINEAR;
static u32 g_renderUs = 0;
static u32 g_mapUs = 0;       /* 渲染分段耗时: 映射 */
static u32 g_interpUs = 0;   /* 渲染分段耗时: 插值+查色 */
static u32 g_fillUs = 0;
/* 功能 3: 高低温追踪状态 */
#define TRACK_CROSS_COLD 0x07FFu      /* 青色准心(铁红 LUT 无青色) */
static float s_hx = 0, s_hy = 0;      /* 高温准心输出像素坐标(平滑) */
static float s_lx = 0, s_ly = 0;      /* 低温准心输出像素坐标(平滑) */
static u8   s_trackInit = 0;
/* 功能 4: 调色板名表 */
static const char *s_palNames[PAL_NUM] = { "IRON", "RAIN", "GRAY" };
/* 功能 6: 超温预警 */
#define ALARM_THRESHOLD 50.0f   /* 触发阈值 °C */
#define ALARM_HYST      2.0f    /* 迟滞: 解除阈值 = 50-2 = 48°C */
static u8 s_alarm = 0;
/* 功能 5: 冻结与拍照 */
static FATFS s_fs;
static FIL   s_fil;
static u32   s_shotNum = 0;    /* 本次运行内递增, 启动时扫描初始化 */
static u8    s_frozen = 0;          /* 0=未报警 1=报警中 */          /* 首帧直接跳变, 避免从(0,0)滑入 */     /* 渲染分段耗时: 刷屏 */

/* 静态 UI: 边框 + 铁红色带 + 标题(只画一次, 每帧只刷新温度文字与色带数值) */
static void Thermal_DrawUI(void)
{
    u16 i;
    POINT_COLOR = WHITE;
    LCD_Fill(0, 0, 255, 1, WHITE);          /* 上边框 */
    LCD_Fill(0, 190, 255, 191, WHITE);      /* 下边框 */
    LCD_Fill(0, 0, 1, 191, WHITE);          /* 左边框 */
    LCD_Fill(254, 0, 255, 191, WHITE);      /* 右边框 */
    for(i = 0; i < 160; i++)                 /* 铁红色带: 160 行映射 256 色阶 */
        LCD_Fill(270, 20 + i, 277, 20 + i, Palette_LookupColor((u8)(i * 255 / 159)));
    LCD_ShowString(10, 205, 200, 16, 16, "MLX90640 THERMAL");
}

/* 十字准心: 臂长 5px 线宽 1px, LCD_Fill 细条(屏 DrawLine 不可靠), 钳到热图区 */
static void Track_DrawCross(int cx, int cy, u16 color)
{
    int x0 = cx - 5, x1 = cx + 5, y0 = cy - 5, y1 = cy + 5;
    if(cx < 0) cx = 0;  if(cx > 255) cx = 255;
    if(cy < 0) cy = 0;  if(cy > 191) cy = 191;
    if(x0 < 0) x0 = 0;  if(x1 > 255) x1 = 255;
    if(y0 < 0) y0 = 0;  if(y1 > 191) y1 = 191;
    LCD_Fill((u16)x0, (u16)cy, (u16)x1, (u16)cy, color);
    LCD_Fill((u16)cx, (u16)y0, (u16)cx, (u16)y1, color);
}

/* 热图边框重画: alarm=1 红 0 白(仅状态切换时调用) */
static void Alarm_DrawBorder(u8 alarm)
{
    u16 c = alarm ? RED : WHITE;
    LCD_Fill(0, 0, 255, 1, c);
    LCD_Fill(0, 190, 255, 191, c);
    LCD_Fill(0, 0, 1, 191, c);
    LCD_Fill(254, 0, 255, 191, c);
}

/* 功能 5: 冻结当前画面并保存 BMP(从 s_idx 逐行重渲染, 与屏上一致) */
static u8 Capture_SaveBmp(void)
{
    char name[13];
    u8 row[768];               /* 一行 RGB888 */
    u16 stripY;
    int line, x;
    UINT bw;
    FRESULT fr;

    sprintf(name, "%04d.BMP", (int)s_shotNum + 1);
    fr = f_open(&s_fil, name, FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK) return 0;
    Bmp_WriteHeader(&s_fil, 256, 192);
    /* BMP 自底向上: 从最底条带倒序写 */
    for(stripY = INTERP_OUT_ROWS - INTERP_STRIP_H; ; stripY -= INTERP_STRIP_H)
    {
        Interp_RenderStrip(stripY, s_idx, s_mode, s_idxStrip);
        Palette_ColorizeStrip(s_idxStrip, s_rgbStrip);
        for(line = INTERP_STRIP_H - 1; line >= 0; line--)
        {
            for(x = 0; x < 256; x++)
            {
                u16 c = s_rgbStrip[line * 256 + x];
                u8 r5 = (u8)((c >> 11) & 0x1F), g6 = (u8)((c >> 5) & 0x3F), b5 = (u8)(c & 0x1F);
                /* BMP 24bpp 字节序为 BGR: 先蓝后绿后红 */
                row[x * 3]     = (u8)((b5 * 255 + 15) / 31);
                row[x * 3 + 1] = (u8)((g6 * 255 + 31) / 63);
                row[x * 3 + 2] = (u8)((r5 * 255 + 15) / 31);
            }
            f_write(&s_fil, row, 768, &bw);
        }
        if(stripY == 0) break;
    }
    f_close(&s_fil);
    s_shotNum++;
    printf("SAVED %s (%d bytes)\r\n", name, 54 + 256 * 192 * 3);
    return 1;
}

int main(void)
{
    int err, lastErr;
    u16 frameCnt = 0;
    u16 i;
    float ta, tmin, tmax, tmid;
    char buf[64];

    delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    uart_init(115200);
    LED_Init();
    KEY_Init();
    LCD_Init();
    BEEP_Init();

    /* 功能 5: SD 卡初始化 */
    {
        SD_CardInfo ci;
        if(SD_Init() == 0)
        {
            SD_GetCardInfo(&ci);
            printf("SD OK, size=%dMB\r\n", (int)(ci.CardCapacity / 1048576u));
        }
        else
        {
            printf("SD init FAIL\r\n");
        }
    }
    Interp_Init();
    DEMCR |= DEMCR_TRCENA;                  /* DWT 计时使能(渲染耗时统计) */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1;

    /* 功能 5: FATFS 挂载 + 已有 BMP 编号扫描 */
    {
        DIR dir;
        FILINFO fi;
        u32 maxnum = 0;
        if(f_mount(&s_fs, "0:", 1) == FR_OK)
        {
            printf("FATFS mount OK\r\n");
            if(f_opendir(&dir, "0:") == FR_OK)
            {
                while(f_readdir(&dir, &fi) == FR_OK && fi.fname[0])
                {
                    u32 n = 0;
                    int i, ok = 1;
                    for(i = 0; i < 8 && fi.fname[i]; i++)
                    {
                        if(fi.fname[i] >= '0' && fi.fname[i] <= '9') n = n * 10 + (u32)(fi.fname[i] - '0');
                        else if(fi.fname[i] == '.') break;
                        else { ok = 0; break; }
                    }
                    if(ok && n > maxnum) maxnum = n;
                }
                s_shotNum = maxnum;
                printf("BMP max num = %d\r\n", (int)maxnum);
            }
        }
        else
        {
            printf("FATFS mount FAIL\r\n");
        }
    }

    POINT_COLOR = WHITE;
    BACK_COLOR = BLACK;                      /* 画文字前背景必须为黑 */
    LCD_Clear(BLACK);
    Thermal_DrawUI();

    LCD_ShowString(10, 205, 200, 16, 16, "MLX90640 INIT...");
    IIC_Init();
    err = MLX90640_Init();

    if(err != MLX_OK)
    {
        sprintf(buf, "MLX90640 FAIL:%d", err);
        LCD_ShowString(10, 220, 200, 16, 16, (u8 *)buf);
        printf("MLX90640 init FAIL, code=%d\r\n", err);
        if(err == -MLX_ERR_I2C_NACK)
            printf("  -> I2C NACK: check wiring (PB6=SCL PB7=SDA)\r\n");
        if(err == -MLX_ERR_NO_DEVICE)
            printf("  -> EEPROM 0x240F addr byte mismatch (expect 0x33)\r\n");
        while(1)
        {
            LED0 = !LED0;
            delay_ms(300);
        }
    }

    printf("MLX90640 found, EE[0x240F]=0x%04X\r\n", (unsigned)g_rawBuf[15]);
    printf("resolutionEE=%d gainEE=%d mode=%d(1=chess)\r\n",
           (int)g_mlx.resolutionEE, (int)g_mlx.gainEE,
           MLX90640_GetCurMode(MLX90640_ADDR));
    LCD_ShowString(10, 205, 200, 16, 16, "ID OK 4Hz RUNNING");
    LCD_ShowString(10, 220, 200, 16, 16, "MODE:BILINEAR PAL:IRON");
    for(i = 0; i < 3; i++)                             /* 刷新率切换后前几帧可能含未积分的 0 值像素, 丢到干净为止 */
    {
        float *p;
        int bad;
        err = MLX90640_GetFrameData(MLX90640_ADDR, g_rawBuf);
        if(err != MLX_OK) continue;
        MLX90640_CalculateTo(g_rawBuf, &g_mlx, MLX_EMISSIVITY, MLX_TREFL, g_to);
        bad = 0;
        for(p = g_to; p < g_to + 768; p++)
            if(*p == 0.0f) { bad = 1; break; }
        if(!bad) break;
    }
    lastErr = MLX_OK;

    while(1)
    {
        if(s_frozen)
        {
            if(KEY_Scan(1) == WKUP_PRES)      /* 再按解冻 */
            {
                s_frozen = 0;
                BEEP = 1; delay_ms(200); BEEP = 0;
                MLX90640_SynchFrame(MLX90640_ADDR);   /* 丢弃冻结期间的过期就绪帧, 防解冻瞬间 -8 */
                printf("RESUME\r\n");
            }
            continue;
        }
        if(KEY_Scan(1) == WKUP_PRES)          /* 一键冻结+拍照 */
        {
            s_frozen = 1;
            if(Capture_SaveBmp()) { BEEP = 1; delay_ms(200); BEEP = 0; }
            else printf("SAVE FAIL\r\n");
        }
        err = MLX90640_GetFrameData(MLX90640_ADDR, g_rawBuf);
        if(err != MLX_OK)
        {
            if(err != lastErr)               /* 状态变化才打印, 避免刷屏 */
            {
                lastErr = err;
                sprintf(buf, "FRAME ERR:%d", err);
                LCD_ShowString(10, 220, 200, 16, 16, (u8 *)buf);
                printf("GetFrameData err=%d\r\n", err);
            }
            delay_ms(100);
            continue;
        }
        lastErr = MLX_OK;

        MLX90640_CalculateTo(g_rawBuf, &g_mlx, MLX_EMISSIVITY, MLX_TREFL, g_to);
        MLX90640_BadPixelsCorrection(g_mlx.outlierPixels, g_to, 1, &g_mlx);
        MLX90640_BadPixelsCorrection(g_mlx.brokenPixels, g_to, 1, &g_mlx);
        MLX90640_ToFixed(s_toFixed, g_to);

        ta = MLX90640_GetTa(g_rawBuf, &g_mlx);
        tmin = g_to[0];
        tmax = g_to[0];
        for(i = 1; i < 768; i++)
        {
            if(g_to[i] < tmin) tmin = g_to[i];
            if(g_to[i] > tmax) tmax = g_to[i];
        }
        tmid = g_to[368];

        /* 功能 6: 超温预警(施密特迟滞: 50 进 48 出) */
        if(!s_alarm && tmax >= ALARM_THRESHOLD)
        {
            s_alarm = 1;
            Alarm_DrawBorder(1);
            printf("ALARM ON tmax=%.1f\r\n", (double)tmax);
        }
        else if(s_alarm && tmax <= ALARM_THRESHOLD - ALARM_HYST)
        {
            s_alarm = 0;
            Alarm_DrawBorder(0);
            printf("ALARM OFF\r\n");
        }
        if(s_alarm) BEEP = (frameCnt & 2u) ? 1 : 0;   /* ~2Hz 断续 */
        else BEEP = 0;                    /* 中心像素(11,16) */

        /* 功能 2: 色阶映射 + 插值渲染(条带) */
        {
            u32 cyc0 = DWT_CYCCNT, t1, t2, t3;
            Palette_MapIdx(g_to, tmin, tmax, s_idx);
            t1 = DWT_CYCCNT;
            for(i = 0; i < INTERP_OUT_ROWS; i += INTERP_STRIP_H)
            {
                Interp_RenderStrip(i, s_idx, s_mode, s_idxStrip);
                Palette_ColorizeStrip(s_idxStrip, s_rgbStrip);
            }
            t2 = DWT_CYCCNT;
            for(i = 0; i < INTERP_OUT_ROWS; i += INTERP_STRIP_H)
            {
                LCD_Color_Fill(0, i, 255, i + INTERP_STRIP_H - 1, s_rgbStrip);
            }
            /* 功能 3: 高低温追踪(热图刚刷完, 旧准心已被覆盖) */
            {
                int hx, hy, lx, ly;
                Track_Find(g_to, &hx, &hy, &lx, &ly);
                if(!s_trackInit)
                {
                    s_hx = (float)(hx * 8 + 4); s_hy = (float)(hy * 8 + 4);
                    s_lx = (float)(lx * 8 + 4); s_ly = (float)(ly * 8 + 4);
                    s_trackInit = 1;
                }
                else
                {
                    Track_Smooth(&s_hx, &s_hy, (float)(hx * 8 + 4), (float)(hy * 8 + 4));
                    Track_Smooth(&s_lx, &s_ly, (float)(lx * 8 + 4), (float)(ly * 8 + 4));
                }
                Track_DrawCross((int)s_hx, (int)s_hy, WHITE);
                if(!(hx == lx && hy == ly))       /* 均匀场只画高温准心 */
                    Track_DrawCross((int)s_lx, (int)s_ly, (u16)TRACK_CROSS_COLD);
            }

            t3 = DWT_CYCCNT;
            for(i = 0; i < 160; i++)         /* 色带(每帧刷新, 跟随 tmax/tmin 动态) */
                LCD_Fill(270, 20 + i, 277, 20 + i, Palette_LookupColor((u8)(i * 255 / 159)));
            sprintf(buf, "%4.1f", tmax);
            LCD_ShowString(268, 2, 52, 16, 16, (u8 *)buf);
            sprintf(buf, "%4.1f", tmin);
            LCD_ShowString(268, 182, 52, 16, 16, (u8 *)buf);
            g_mapUs    = (t1 - cyc0) / 72u;
            g_interpUs = (t2 - t1) / 72u;
            g_fillUs   = (t3 - t2) / 72u;
            g_renderUs = (t3 - cyc0) / 72u;   /* 72MHz -> us */
        }

        /* KEY0: 循环切换调色板 / KEY1: 切换插值模式(对比实验开关) */
        if(KEY_Scan(1) == KEY0_PRES)
        {
            Palette_Select((PaletteId)((Palette_Current() + 1) % PAL_NUM));
        }
        if(KEY_Scan(1) == KEY1_PRES)
        {
            s_mode = (s_mode == INTERP_MODE_BILINEAR) ? INTERP_MODE_NEAREST : INTERP_MODE_BILINEAR;
        }
        sprintf(buf, "MODE:%s PAL:%s",
                (s_mode == INTERP_MODE_BILINEAR) ? "BILINEAR" : "NEAREST",
                s_palNames[Palette_Current()]);
        LCD_ShowString(10, 220, 200, 16, 16, (u8 *)buf);

        frameCnt++;
        if(frameCnt % 8 == 1)                /* 4Hz 时约 1Hz 输出 */
        {
            printf("F=%d sub=%d Ta=%.1f min=%.1f max=%.1f mid=%.1f r=%dms\r\n",
                   (int)frameCnt, (int)g_rawBuf[FRAME_SUBPAGE],
                   (double)ta, (double)tmin, (double)tmax, (double)tmid,
                   (int)(g_renderUs / 1000u));
            printf("  map=%dms interp=%dms fill=%dms\r\n",
                   (int)(g_mapUs / 1000u), (int)(g_interpUs / 1000u), (int)(g_fillUs / 1000u));
            sprintf(buf, "Ta:%5.1f min:%5.1f max:%5.1f", ta, tmin, tmax);
            LCD_ShowString(10, 205, 240, 16, 16, (u8 *)buf);
        }
        LED0 = !LED0;
    }
}
