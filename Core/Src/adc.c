/* 寄存器级 ADC1 + IWDG + WWDG 驱动（机器缺 HAL 源文件，故手写，与 oled.c/spi.c 风格一致）
 *
 * ADC 时钟：APB2 = 84MHz，经 ADC_CCR.ADCPRE 分频后必须 ≤ 36MHz -> 选 /4 = 21MHz。
 * 内部温度传感器挂在 ADC1_CH16，需置 ADC_CCR.TSVREFE=1 才供电；VREFINT 在 CH17 同受该位控制。
 * 量化：12 位 -> 0..4095，VREF+=VDDA=3.3V。温度公式：Temp = (VSENSE-0.76)/0.0025 + 25。
 */

#include "main.h"
#include "adc.h"

/* ===================== ADC1 ===================== */

void ADC1_Init(void)
{
    /* 1) 开 ADC1 时钟（APB2） */
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    /* 2) 公共控制寄存器 ADC_CCR：
     *    ADCPRE[17:16] = 01 -> /4（84M/4 = 21MHz ≤ 36MHz 上限）
     *    TSVREFE (bit23) = 1 -> 给内部温度传感器 + VREFINT 供电
     */
    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | (0x1u << 16);
    ADC->CCR |= ADC_CCR_TSVREFE;

    /* 3) CR1：RES[25:24]=00 -> 12 位分辨率 */
    ADC1->CR1 &= ~ADC_CR1_RES;

    /* 4) CR2：右对齐 + 单次转换(SWSTART 触发) + 每次转换后 EOC 置位(EOCS=1) */
    ADC1->CR2 &= ~ADC_CR2_ALIGN;
    ADC1->CR2 &= ~ADC_CR2_CONT;
    ADC1->CR2 |=  ADC_CR2_EOCS;

    /* 5) 采样时间：CH16 用最长 480 周期（温度传感器要求慢采样才准） */
    ADC1->SMPR1 |= ADC_SMPR1_SMP16;   /* SMP16[20:18] = 111 */

    /* 6) 规则序列：1 次转换，SQ1 = 通道 16 */
    ADC1->SQR1 &= ~ADC_SQR1_L;                       /* L[23:20] = 0 -> 1 次 */
    ADC1->SQR3  = (ADC1->SQR3 & ~ADC_SQR3_SQ1) | (16u << 0);

    /* 7) 使能 ADC，等待稳定后丢弃首次转换（传感器刚上电首次不准） */
    ADC1->CR2 |= ADC_CR2_ADON;
    HAL_Delay(1);                 /* t_STAB 典型几 µs，给 1ms 冗余 */
    ADC1_ReadChannel(16);         /* dummy read */
}

/* 触发一次规则转换并轮询读回（带超时，避免从机/配置异常时死等） */
uint16_t ADC1_ReadChannel(uint8_t ch)
{
    ADC1->SQR3 = (ADC1->SQR3 & ~ADC_SQR3_SQ1) | ((uint32_t)ch << 0);
    ADC1->CR2 |= ADC_CR2_SWSTART;

    uint32_t t0 = HAL_GetTick();
    while (!(ADC1->SR & ADC_SR_EOC)) {
        if (HAL_GetTick() - t0 > 10) return 0;   /* 10ms 超时兜底 */
    }
    return (uint16_t)(ADC1->DR & 0x0FFF);
}

uint16_t ADC1_ReadTempSensor(void) { return ADC1_ReadChannel(16); }
uint16_t ADC1_ReadVrefint(void)    { return ADC1_ReadChannel(17); }

float ADC1_ToTempC(uint16_t adc_val)
{
    float v = (float)adc_val * 3.3f / 4095.0f;   /* VDDA = 3.3V */
    return (v - 0.76f) / 0.0025f + 25.0f;        /* V25=0.76V, Avg_Slope=2.5mV/℃ */
}

/* ===================== IWDG（独立看门狗） ===================== */

void IWDG_Init(void)
{
    IWDG->KR = 0x5555;                 /* 解除写保护，允许改 PR/RLR */
    IWDG->PR = 4;                      /* 预分频 /64（PR[2:0]=100） */
    IWDG->RLR = 2000;                  /* 重装载 2000 -> LSI(~32k)/64*2001 ≈ 4s 超时（宽松，避免误触发） */
    uint32_t t = 100000;
    while ((IWDG->SR & 0x03) && --t);  /* 等 PVU(bit0)+RVU(bit1) 清，带超时保护 */
    IWDG->KR = 0xAAAA;                 /* 装载重装载值 */
    IWDG->KR = 0xCCCC;                 /* 启动看门狗（开始递减） */
}

void IWDG_Feed(void)
{
    IWDG->KR = 0xAAAA;                 /* 重装载，喂狗 */
}

/* ===================== WWDG（窗口看门狗） ===================== */

void WWDG_Init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_WWDGEN;        /* 开 WWDG 时钟（PCLK1） */
    /* CFR：WDGTB[8:7]=3(/8)，W[6:0]=0x7F
     * 超时 = (T+1)*4096*2^WDGTB / PCLK1 = 128*4096*8 / 42000000 ≈ 100ms
     * 注意：窗口看门狗的“早喂复位”条件是“计数器还很大(>W)时就喂狗”。
     * 若 W 设小(如 0x55)，主循环首圈喂狗时计数器仍很大 -> 立刻复位 -> 死循环。
     * 这里把 W 拉到与计数器上限相同(0x7F)，窗口下限消失，退化为“仅超时复位”，
     * 任何时刻(未超时前)喂狗都安全，先保证程序稳定运行。
     * 想体验真正的窗口特性：把 W 改小(如 0x55)并让主循环喂狗间隔落在开放窗口内。
     */
    WWDG->CFR = (3u << 7) | (0x7Fu << 0);
    WWDG->CR  = (0x7Fu << 0) | 0x80u;         /* T=0x7F，WDGA(bit7)=1 激活看门狗 */
}

void WWDG_Feed(void)
{
    WWDG->CR = (0x7Fu << 0) | 0x80u;          /* 重载 T=0x7F，保持激活 */
}
