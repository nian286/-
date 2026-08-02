/* SPI1 寄存器级驱动（Mode 0 主模式，回环自测用）
 * - PA5=SCK, PA7=MOSI, PA6=MISO, PA4=CS（均 AF5 复用，软件管理 NSS）
 * - 机器上缺 stm32f4xx_hal_spi.c，故手写寄存器级（与 oled.c 的 I2C 寄存器驱动风格一致）
 * - 时钟：SPI1 挂在 APB2 = 84MHz，波特率 /64 ≈ 1.31MHz（便于逻辑分析仪看波形）
 */
#include <stdio.h>
#include "main.h"
#include "spi.h"

/* SPI1 寄存器（F407 参考手册，基地址 0x40013000） */
#define SPI1_CR1  (*(volatile uint32_t *)(0x40013000 + 0x00))
#define SPI1_CR2  (*(volatile uint32_t *)(0x40013000 + 0x04))
#define SPI1_SR   (*(volatile uint32_t *)(0x40013000 + 0x08))
#define SPI1_DR   (*(volatile uint32_t *)(0x40013000 + 0x0C))

/* 注：SPI_CR1_ 与 SPI_SR_ 等位域宏由 CMSIS 设备头(stm32f4xx.h, via main.h)已定义，直接复用，不再重定义 */

void SPI1_Init(void)
{
    /* 1) 开时钟：GPIOA（MX_GPIO_Init 已开） + SPI1(APB2 位12) */
    RCC->APB2ENR |= (1u << 12);

    /* 2) PA4 = CS，输出推挽，初始高（不选中） */
    GPIOA->MODER   &= ~(3u << (4 * 2));
    GPIOA->MODER   |=  (1u << (4 * 2));       /* output */
    GPIOA->OSPEEDR |=  (3u << (4 * 2));
    GPIOA->BSRR     =  (1u << 4);             /* CS 拉高（不选中） */

    /* 3) PA5(SCK) / PA6(MISO) / PA7(MOSI) -> 复用 AF5（SPI1） */
    GPIOA->MODER   &= ~((3u << (5 * 2)) | (3u << (6 * 2)) | (3u << (7 * 2)));
    GPIOA->MODER   |=  ((2u << (5 * 2)) | (2u << (6 * 2)) | (2u << (7 * 2)));
    GPIOA->OTYPER  &= ~((1u << 5) | (1u << 6) | (1u << 7));   /* push-pull */
    GPIOA->OSPEEDR |=  ((3u << (5 * 2)) | (3u << (6 * 2)) | (3u << (7 * 2)));  /* very high */
    GPIOA->PUPDR   &= ~((3u << (5 * 2)) | (3u << (6 * 2)) | (3u << (7 * 2)));  /* no pull */
    /* AF5：pin5/6/7 全部在 AFRL(AFR[0])（AFRH 只管 pin8-15，此处用不到）
     * 错例：pa6/pa7 写成 AFR[1] 会导致复用功能未生效，MISO/MOSI 接不到 SPI1 */
    GPIOA->AFR[0]  &= ~((0xFu << (5 * 4)) | (0xFu << (6 * 4)) | (0xFu << (7 * 4)));
    GPIOA->AFR[0]  |=  ((5u  << (5 * 4)) | (5u  << (6 * 4)) | (5u  << (7 * 4)));

    /* 4) SPI1 配置：Mode0(CPOL=0,CPHA=0)、主机、8位、BR=/64、软件 NSS */
    SPI1_CR1 &= ~SPI_CR1_SPE;     /* 关外设再写配置 */
    SPI1_CR1  = 0;
    SPI1_CR1 |= SPI_CR1_MSTR;     /* 主机模式 */
    SPI1_CR1 |= (5u << 3);        /* BR = /64  -> 84MHz/64 ≈ 1.31MHz */
    SPI1_CR1 |= SPI_CR1_SSM;      /* 软件管理 NSS（避免依赖外部 CS 引脚） */
    SPI1_CR1 |= SPI_CR1_SSI;      /* SSI=1，内部拉高 NSS，避免 MODF 模式错误 */
    /* CPOL/CPHA 保持 0 -> Mode 0（上升沿采样） */
    SPI1_CR1 |= SPI_CR1_SPE;      /* 使能外设 */
}

/* 全双工收发 1 字节（Mode 0，轮询）：发 tx 的同时从 MISO 收回 1 字节 */
uint8_t SPI1_TransmitReceive(uint8_t tx)
{
    while (!(SPI1_SR & SPI_SR_TXE));     /* 等发送缓冲空 */
    SPI1_DR = tx;                        /* 写 = 发出去 */
    while (!(SPI1_SR & SPI_SR_RXNE));    /* 等接收缓冲有数据 */
    return (uint8_t)SPI1_DR;             /* 读 = 收回（清 RXNE） */
}

/* 回环自测：MOSI(PA7) 短接 MISO(PA6) 时，发出去的字节应收回相同字节 */
int SPI1_LoopbackTest(void)
{
    const uint8_t txbuf[] = {0xAA, 0x55, 0x00, 0xFF, 0x12, 0x34, 0x56, 0x78};
    int ok = 1;
    for (int i = 0; i < (int)(sizeof(txbuf)); i++) {
        uint8_t tx = txbuf[i];
        uint8_t rx;
        GPIOA->BSRR = (1u << (4 + 16));   /* CS 拉低（开始，高16位写1=清输出） */
        rx = SPI1_TransmitReceive(tx);    /* 全双工收发 */
        while (SPI1_SR & SPI_SR_BSY);     /* 等移位完成 */
        GPIOA->BSRR = (1u << 4);          /* CS 拉高（结束） */
        if (rx != tx) {
            ok = 0;
            printf("SPI loopback ERR: tx=0x%02X rx=0x%02X\r\n", tx, rx);
        } else {
            printf("SPI loopback OK : tx=rx=0x%02X\r\n", tx);
        }
    }
    if (ok) printf("SPI1 loopback PASSED\r\n");
    else    printf("SPI1 loopback FAILED\r\n");
    return ok;
}
