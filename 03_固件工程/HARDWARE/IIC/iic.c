#include "iic.h"
#include "delay.h"

/*
 * 软件模拟 I2C（bit-bang），PB6=SCL / PB7=SDA。
 *
 * 改为软件模拟的原因（2026-10-03 硬件联调记录）：
 * 板载硬件 I2C1 外设对 START 指令完全无响应——时钟已开(APB1ENR bit21=1)、
 * 配置寄存器正确(CR1=0x0401/CR2=0x0024/CCR=0x801E/TRISE=0x000B)、
 * 总线空闲且被上拉(IDR 读回高电平)，SWRST 软件复位后仍不发 START，
 * SB/BUSY 标志从不置位，疑似芯片 I2C 外设硅片级失效。
 * 改用 GPIO 模拟后接口与错误码不变，上层 MLX90640 驱动零改动。
 */

#define IIC_SCL_H()  (GPIOB->BSRR = (1u << 6))
#define IIC_SCL_L()  (GPIOB->BRR  = (1u << 6))
#define IIC_SDA_H()  (GPIOB->BSRR = (1u << 7))
#define IIC_SDA_L()  (GPIOB->BRR  = (1u << 7))
#define IIC_SDA_IN() ((GPIOB->IDR >> 7) & 1u)

/* PB7 方向切换：读时为上拉输入，写时为推挽输出 */
static void IIC_SDA_Input(void)
{
    GPIOB->CRL &= 0x0FFFFFFFu;
    GPIOB->CRL |= 0x80000000u;    /* 1000: 输入 + 上拉(ODR7=1 时) */
}

static void IIC_SDA_Output(void)
{
    GPIOB->CRL &= 0x0FFFFFFFu;
    GPIOB->CRL |= 0x30000000u;    /* 0011: 推挽输出 50MHz */
}

/* 起始条件：SCL 高电平期间 SDA 下跳 */
static void IIC_Start(void)
{
    IIC_SDA_Output();
    IIC_SDA_H();
    IIC_SCL_H();
    delay_us(4);
    IIC_SDA_L();
    delay_us(4);
    IIC_SCL_L();
}

/* 停止条件：SCL 高电平期间 SDA 上跳 */
static void IIC_Stop(void)
{
    IIC_SDA_Output();
    IIC_SCL_L();
    IIC_SDA_L();
    IIC_SCL_H();
    delay_us(4);
    IIC_SDA_H();
    delay_us(4);
}

/* 发送一个字节(含第 9 个时钟读应答)，返回 0=ACK / 1=NACK */
static u8 IIC_SendByte(u8 dat)
{
    u8 i;
    IIC_SDA_Output();
    for(i = 0; i < 8; i++)
    {
        IIC_SCL_L();
        if(dat & 0x80) IIC_SDA_H(); else IIC_SDA_L();
        delay_us(2);
        IIC_SCL_H();
        delay_us(2);
        dat <<= 1;
    }
    /* 第 9 个时钟：释放 SDA 读应答 */
    IIC_SCL_L();
    IIC_SDA_H();
    IIC_SDA_Input();
    delay_us(2);
    IIC_SCL_H();
    delay_us(2);
    i = (u8)IIC_SDA_IN();
    IIC_SCL_L();
    return i;
}

/* 读一个字节；ack=1 回 ACK，ack=0 回 NACK(最后一字节) */
static u8 IIC_ReadByte(u8 ack)
{
    u8 i, dat = 0;
    IIC_SDA_H();          /* 置 ODR 使上拉输入生效 */
    IIC_SDA_Input();
    for(i = 0; i < 8; i++)
    {
        IIC_SCL_L();
        delay_us(2);
        IIC_SCL_H();
        dat <<= 1;
        if(IIC_SDA_IN()) dat |= 1;
        delay_us(2);
    }
    IIC_SDA_Output();
    IIC_SCL_L();
    if(ack) IIC_SDA_L(); else IIC_SDA_H();
    delay_us(2);
    IIC_SCL_H();
    delay_us(2);
    IIC_SCL_L();
    IIC_SDA_H();
    return dat;
}

/* 硬件 I2C1 初始化(兼容原接口): 引脚改推挽输出, 总线置空闲 */
void IIC_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    IIC_SCL_H();
    IIC_SDA_H();          /* 总线空闲；ODR7=1 同时为读态选上拉 */
}

/* 从 dev 的 16 位寄存器地址 reg 连续读 n 字节。
 * 总线格式同硬件版: 写地址+寄存器高/低字节 -> 重复起始 -> 读地址 -> 数据。
 * 返回: IIC_OK / IIC_NACK(任一步骤无应答) */
u8 IIC_ReadBytes(u8 dev, u16 reg, u16 n, u8 *buf)
{
    u16 i;

    IIC_Start();
    if(IIC_SendByte((u8)(dev << 1)))     goto nack;   /* 写地址 */
    if(IIC_SendByte((u8)(reg >> 8)))     goto nack;   /* 寄存器高字节 */
    if(IIC_SendByte((u8)(reg & 0xFF)))   goto nack;   /* 寄存器低字节 */

    IIC_Start();                                       /* 重复起始 */
    if(IIC_SendByte((u8)((dev << 1) | 1))) goto nack;  /* 读地址 */

    for(i = 0; i < n; i++)
        buf[i] = IIC_ReadByte(i != n - 1);             /* 除末字节外回 ACK */

    IIC_Stop();
    return IIC_OK;

nack:
    IIC_Stop();
    return IIC_NACK;
}

/* 向 dev 的 16 位寄存器地址 reg 写一个 16 位字(先高字节后低字节) */
u8 IIC_WriteWord(u8 dev, u16 reg, u16 data)
{
    IIC_Start();
    if(IIC_SendByte((u8)(dev << 1)))    goto nack;
    if(IIC_SendByte((u8)(reg >> 8)))    goto nack;
    if(IIC_SendByte((u8)(reg & 0xFF)))  goto nack;
    if(IIC_SendByte((u8)(data >> 8)))   goto nack;
    if(IIC_SendByte((u8)(data & 0xFF))) goto nack;

    IIC_Stop();
    return IIC_OK;

nack:
    IIC_Stop();
    return IIC_NACK;
}
