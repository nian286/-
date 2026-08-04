#ifndef __ADC_H
#define __ADC_H

#include "main.h"

/* ---------- ADC1（寄存器级，单/多通道轮询） ---------- */
void     ADC1_Init(void);                 // 时钟+公共控制+温度传感器使能+单次12位配置
uint16_t ADC1_ReadChannel(uint8_t ch);    // 触发一次转换并读回 12 位原始值（带超时保护）
uint16_t ADC1_ReadTempSensor(void);       // 内部温度传感器 CH16
uint16_t ADC1_ReadVrefint(void);          // 内部基准 VREFINT CH17
float    ADC1_ToTempC(uint16_t adc_val);  // 原始值 -> 摄氏温度（VDDA=3.3V, V25=0.76V, 斜率2.5mV/℃）

/* ---------- 看门狗 ---------- */
void IWDG_Init(void);   // 独立看门狗：LSI~32k, /64, 重载124 -> 超时≈1s
void IWDG_Feed(void);   // 喂狗（写 0xAAAA）
void WWDG_Init(void);   // 窗口看门狗：PCLK1=42M, /8, T=0x7F, W=0x55 -> 超时≈50ms, 窗口≈[33,50]ms
void WWDG_Feed(void);   // 喂狗（重载 T，保持 WDGA）

#endif /* __ADC_H */
