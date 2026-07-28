#ifndef __OLED_H
#define __OLED_H

#include <stdint.h>

/* OLED 的 I2C 从机地址（7-bit）。
 * 模块背面地址选择焊点默认 0x3C；若被改过则是 0x3D，按需修改。 */
#define OLED_I2C_ADDR  0x3C

#define OLED_W         128
#define OLED_H         64
#define OLED_BUF_SIZE  (OLED_W * OLED_H / 8)   /* 1024 字节显存镜像 */

/* ---- 底层 I2C1 主发（寄存器级，不依赖 HAL I2C 驱动源文件） ---- */
void OLED_I2C_Init(void);
int  OLED_I2C_Write(uint8_t addr, const uint8_t *buf, uint16_t len);

/* ---- OLED（SSD1306）驱动 API ---- */
void OLED_Init(void);          /* 初始化 SSD1306 + 清屏 + 刷新 */
void OLED_Clear(void);         /* 显存全清（黑） */
void OLED_Fill(int on);        /* 显存全填充（on=1 全亮） */
void OLED_Refresh(void);       /* 把显存镜像整屏推到 OLED */
void OLED_DrawPixel(int x, int y, int on);
void OLED_DrawLine(int x0, int y0, int x1, int y1, int on);

/* ---- 6x8 ASCII 文字 ---- */
/* 在 (x,y) 画一个字符。字符宽 6 像素(5 实际 + 1 间隔)、高 8 像素(占 1 页)。
 * y 必须是 8 的倍数(因为字模按页排版);x 超出会被裁掉。 */
void OLED_DrawChar(int x, int y, char c);
/* 在 (x,y) 写一个 \0 结尾的字符串。 */
void OLED_DrawString(int x, int y, const char *s);

extern uint8_t oled_buf[OLED_BUF_SIZE];

#endif /* __OLED_H */
