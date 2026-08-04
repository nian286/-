/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "oled.h"
#include "spi.h"
#include "adc.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
DMA_HandleTypeDef hdma_usart1_rx;          // USART1_RX 的 DMA 句柄（手动配置）

#define DMA_BUF_SIZE 64
uint8_t dma_buf[DMA_BUF_SIZE];             // DMA 搬运目的地（底层缓冲）

// ---- 环形缓冲：解耦“DMA 收(生产者/ISR)”与“main 解析(消费者)” ----
#define RB_SIZE 128
typedef struct {
    uint8_t  buf[RB_SIZE];
    volatile uint16_t head;                // 写指针（ISR 增长）
    volatile uint16_t tail;                // 读指针（main 增长）
    volatile uint32_t overflow;            // 满时丢帧计数
} ring_buf_t;
ring_buf_t rb;

// ---- 定时器14 + PWM 呼吸灯 ----
TIM_HandleTypeDef htim14;            // 呼吸灯用 TIM14（挂在 APB1）
static uint16_t breath_ccr = 0;     // 当前占空比(0~ARR)，越大 LED0 越亮(active-low+低极性)
static int8_t   breath_dir = 1;     // 1=变亮, -1=变暗

static volatile int wdt_hang = 0;    // 非 0 时停止喂狗，用于演示看门狗复位保护
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static void MX_DMA_Init(void);
static void ring_put(uint8_t c);
static int  ring_get(uint8_t *c);
static void drain_uart(void);
static void process_command(uint8_t *cmd, uint16_t len);
static void MX_TIM14_Init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  // 启动指示：能执行到这里 = HAL_Init + 系统时钟 + GPIO 初始化都成功（板子活着）
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);  // LED1 亮，证明程序已进 main

  // 初始化 DMA 句柄 + 开时钟 + NVIC。
  // ⚠️ 注意：此调用必须放在 USER CODE 块内——CubeMX 的 Generate Code 会重写 main()，
  //    把标准初始化序列之外的手写 MX_* 调用删掉（上一版就是被它删了导致 DMA 句柄未初始化 → HardFault 全灭）。
  MX_DMA_Init();

  // 启动 DMA + IDLE：硬件后台搬字节，仅一帧结束(IDLE)进一次回调，CPU 几乎零打扰
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, dma_buf, DMA_BUF_SIZE) != HAL_OK) {
      Error_Handler();
  }
  printf("UART DMA RX ready, send 'LED ON' / 'LED OFF' (controls LED1)\r\n");

  // ---- 定时器14 + PWM 呼吸灯（LED0 = PF9 = TIM14_CH1） ----
  MX_TIM14_Init();
  // PF9 在 MX_GPIO_Init 里是 GPIO_Output，这里改配为 TIM14_CH1 复用(AF9)
  GPIO_InitTypeDef pg = {0};
  pg.Pin       = LED0_Pin;
  pg.Mode      = GPIO_MODE_AF_PP;
  pg.Pull      = GPIO_NOPULL;
  pg.Speed     = GPIO_SPEED_FREQ_LOW;
  pg.Alternate = GPIO_AF9_TIM14;
  HAL_GPIO_Init(LED0_GPIO_Port, &pg);
  // 启动 PWM 硬件输出：使能 OC 比较通道，PF9 引脚开始输出波形
  if (HAL_TIM_PWM_Start(&htim14, TIM_CHANNEL_1) != HAL_OK) {
      Error_Handler();
  }
  // 关键修复：HAL_TIM_PWM_Start_IT 只开“捕获比较(CC)中断”，并不会开“更新(Update)中断”。
  // 呼吸步进依赖 HAL_TIM_PeriodElapsedCallback（更新中断触发），必须手动把更新中断打开，
  // 否则 breath_ccr 永远卡在 0 → CCR=0 → 引脚整周期输出高电平 → LED0 一直灭。
  __HAL_TIM_ENABLE_IT(&htim14, TIM_IT_UPDATE);
  printf("PWM breath on LED0 ready\r\n");

  // ---- I2C + OLED 显示（PB6=SCL, PB7=SDA，已加外部 4.7K 上拉） ----
  OLED_I2C_Init();
  OLED_Init();
  // 演示：清屏 -> 画边框 + 两条对角线 + 中心实心圆 + 文字
  OLED_Clear();
  for (int x = 0; x < OLED_W; x++) {
      OLED_DrawPixel(x, 0, 1);
      OLED_DrawPixel(x, OLED_H - 1, 1);
  }
  for (int y = 0; y < OLED_H; y++) {
      OLED_DrawPixel(0, y, 1);
      OLED_DrawPixel(OLED_W - 1, y, 1);
  }
  OLED_DrawLine(0, 0, OLED_W - 1, OLED_H - 1, 1);
  OLED_DrawLine(0, OLED_H - 1, OLED_W - 1, 0, 1);
  int cx = OLED_W / 2, cy = OLED_H / 2, r = 18;
  for (int y = -r; y <= r; y++)
      for (int x = -r; x <= r; x++)
          if (x * x + y * y <= r * r) OLED_DrawPixel(cx + x, cy + y, 1);

  /* ---- 文字显示（6x8 ASCII 字模） ----
   * 字模按页排版(8 行高),所以 y 必须是 8 的倍数,这里:
   *   y=0   → 顶部"标题"行
   *   y=24  → 第 4 页,在圆上方打个 HELLO
   *   y=56  → 最底页,在边框内打 MCU 状态
   */
  OLED_DrawString(34, 0,  "I2C OLED");        /* 居中标题 */
  OLED_DrawString(28, 24, "HELLO STM32");     /* 在圆上方 */
  OLED_DrawString(8,  56, "F407 + SSD1306");  /* 最底一行 */
  OLED_Refresh();
  printf("OLED demo drawn (with text)\r\n");

  // ---- SPI1 寄存器级回环自测（PA5=SCK, PA7=MOSI, PA6=MISO, PA4=CS；Mode0, ~1.3MHz） ----
  // 接线：用杜邦线把 PA7(MOSI) 与 PA6(MISO) 短接，即可自己发自己收
  SPI1_Init();
  int spi_ok = SPI1_LoopbackTest();
  OLED_DrawString(8, 48, spi_ok ? "SPI1: PASS" : "SPI1: FAIL");  /* 页6，独立于原图形 */
  OLED_Refresh();
  printf("SPI1 loopback %s\r\n", spi_ok ? "PASSED" : "FAILED");

  // ---- ADC1 寄存器级：读内部温度传感器（零硬件，无需电位器） ----
  ADC1_Init();
  uint16_t adc_raw = ADC1_ReadTempSensor();
  float    adc_temp = ADC1_ToTempC(adc_raw);
  printf("ADC temp sensor: raw=%u  T=%.1fC\r\n", adc_raw, adc_temp);

  // ---- 看门狗：IWDG(独立,~1s) + WWDG(窗口,~50ms) ----
  // 注意：必须在 OLED 刷新之后使能，避免刷新期间 I2C 长阻塞触发 WWDG 复位。
  IWDG_Init();
  WWDG_Init();
  printf("IWDG(~4s) + WWDG(~100ms) armed (send 'HANG' to stop feeding -> auto reset)\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    drain_uart();   // 消费环形缓冲，解析串口命令（LED1 由 “LED ON/OFF” 控制）

    // 心跳：每 1s 打印 tick + 芯片温度，证明主循环在跑
    static uint32_t last_tick = 0;
    if (HAL_GetTick() - last_tick >= 1000) {
        last_tick = HAL_GetTick();
        uint16_t raw = ADC1_ReadTempSensor();
        float temp = ADC1_ToTempC(raw);
        printf("tick=%lu  TEMP=%.1fC (raw=%u)\r\n", last_tick, temp, raw);
    }
    // 喂狗：IWDG 每循环都喂(~10ms << 1s 超时)；WWDG 每 ~40ms 喂一次(落在窗口内)
    if (!wdt_hang) {
        IWDG_Feed();
        static uint32_t last_wdog = 0;
        if (HAL_GetTick() - last_wdog >= 40) {
            last_wdog = HAL_GetTick();
            WWDG_Feed();
        }
    }
    HAL_Delay(10);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */
  // 使能 USART1 全局中断（IDLE 中断靠它进 ISR；上一版没接导致收不到）
  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */
  // 把 DMA 句柄挂到 UART 的 RX 通道（必须在 HAL_UART_Init 之后）
  __HAL_LINKDMA(&huart1, hdmarx, hdma_usart1_rx);
  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOF, LED0_Pin|LED1_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : LED0_Pin LED1_Pin */
  GPIO_InitStruct.Pin = LED0_Pin|LED1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /*Configure GPIO pin : KEY_UP_Pin */
  GPIO_InitStruct.Pin = KEY_UP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;  // WK_UP(PA0) 正点原子标准:按键另一端接VCC,按下=高电平;内部下拉使平时为低,按下产生上升沿触发
  HAL_GPIO_Init(KEY_UP_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
// ---- 环形缓冲基本操作 ----
static uint16_t ring_count(void) { return (rb.head - rb.tail + RB_SIZE) % RB_SIZE; }
static uint16_t ring_free(void)  { return RB_SIZE - 1 - ring_count(); }

static void ring_put(uint8_t c) {
    if (ring_free() == 0) { rb.overflow++; return; }   // 满：丢新保旧，记溢出
    rb.buf[rb.head] = c;
    rb.head = (rb.head + 1) % RB_SIZE;
}
static int ring_get(uint8_t *c) {
    if (ring_count() == 0) return -1;
    *c = rb.buf[rb.tail];
    rb.tail = (rb.tail + 1) % RB_SIZE;
    return 0;
}

// ---- DMA 初始化（手动替代 CubeMX 生成；USART1_RX -> DMA2_Stream2/CH4） ----
static void MX_DMA_Init(void) {
    __HAL_RCC_DMA2_CLK_ENABLE();
    hdma_usart1_rx.Instance                 = DMA2_Stream2;
    hdma_usart1_rx.Init.Channel             = DMA_CHANNEL_4;
    hdma_usart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart1_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode                = DMA_NORMAL;   // 收满或 IDLE 停，回调里重武装
    hdma_usart1_rx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_usart1_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK) { Error_Handler(); }

    HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
}

// ---- 命令解析（消费者，main 上下文） ----
static void process_command(uint8_t *cmd, uint16_t len) {
    if (len >= 6 && strncmp((char *)cmd, "LED ON", 6) == 0) {
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);  // 低电平亮(LED1)
        printf("LED1 ON\r\n");
    } else if (len >= 7 && strncmp((char *)cmd, "LED OFF", 7) == 0) {
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);    // 高电平灭
        printf("LED1 OFF\r\n");
    } else if (len >= 4 && strncmp((char *)cmd, "HANG", 4) == 0) {
        wdt_hang = 1;
        printf("HANG: 停止喂狗，看门狗将在 ~1s 后自动复位系统\r\n");
    } else {
        printf("Unknown: %s\r\n", cmd);
    }
}

// ---- 从环形缓冲解析命令（消费者，main 上下文）----
// 兼容两种发送方式：① 带回车/换行（标准帧结束）② 不带换行，收到完整命令前缀即执行
static void drain_uart(void) {
    static uint8_t line[64];
    static uint16_t li = 0;
    uint8_t c;
    while (ring_get(&c) == 0) {
        if (c == '\r' || c == '\n') {   // 换行：结束本帧（兼容带换行的发送）
            if (li > 0) {
                line[li] = 0;
                process_command(line, li);
                li = 0;
            }
            continue;
        }
        if (li < sizeof(line) - 1) line[li++] = c;
        // 无换行兜底：收到完整命令前缀立即执行，免去“必须发回车”的限制（修复回归）
        if (li >= 7 && strncmp((char *)line, "LED OFF", 7) == 0) {
            process_command(line, 7); li = 0;
        } else if (li >= 6 && strncmp((char *)line, "LED ON", 6) == 0) {
            process_command(line, 6); li = 0;
        } else if (li >= 4 && strncmp((char *)line, "HANG", 4) == 0) {
            process_command(line, 4); li = 0;
        }
    }
}

// ---- 收完一帧（TC 或 IDLE）回调：生产者，ISR 上下文 ----
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance != USART1) return;
    // NORMAL 模式下 HT(半传输) 也会进回调但不停收，忽略它，只处理 TC/IDLE
    if (HAL_UARTEx_GetRxEventType(huart) == HAL_UART_RXEVENT_HT) return;

    for (uint16_t i = 0; i < Size; i++) {
        ring_put(dma_buf[i]);   // 这一帧新到的字节搬进环形缓冲
    }
    // 重武装：继续等下一帧（NORMAL 模式需手动重开）
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, dma_buf, DMA_BUF_SIZE);
}

// ---- 定时器14 初始化：1kHz PWM 载波，CH1 输出到 PF9(LED0) ----
static void MX_TIM14_Init(void) {
    __HAL_RCC_TIM14_CLK_ENABLE();
    htim14.Instance = TIM14;
    htim14.Init.Prescaler = 83;                    // TIM14 在 APB1，定时器时钟=APB1(42M)×2=84MHz；84M/(83+1)=1MHz 计数
    htim14.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim14.Init.Period = 999;                      // 1MHz / 1000 = 1kHz 载波(远高于闪烁融合频率)
    htim14.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim14.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&htim14) != HAL_OK) Error_Handler();

    TIM_OC_InitTypeDef sOC = {0};
    sOC.OCMode = TIM_OCMODE_PWM1;
    sOC.Pulse = 0;                                 // 初始占空比 0
    sOC.OCPolarity = TIM_OCPOLARITY_LOW;           // LED 低电平亮 → 低极性: CCR 越大亮越久
    sOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim14, &sOC, TIM_CHANNEL_1) != HAL_OK) Error_Handler();

    // TIM14 与 TIM8_TRG_COM 共用中断向量
    HAL_NVIC_SetPriority(TIM8_TRG_COM_TIM14_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM8_TRG_COM_TIM14_IRQn);
}

// ---- PWM 更新中断：每周期步进一次占空比，做出三角波呼吸 ----
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM14) {
        breath_ccr += breath_dir;
        if (breath_ccr >= 999) { breath_ccr = 999; breath_dir = -1; }   // 到顶反转
        else if (breath_ccr == 0) { breath_dir = 1; }                   // 到底反转
        __HAL_TIM_SET_COMPARE(&htim14, TIM_CHANNEL_1, breath_ccr);
    }
}

// ---- 按键外部中断回调：KEY_UP(PA0) 触发，软件消抖后翻转 LED1 ----
// 配置：PULLDOWN + IT_RISING，适配 WK_UP(PA0) 按键另一端接 VCC（按下=上升沿）。
// 若实机发现仍是“松手才翻”或“完全没反应”，说明键是“按下接地”接法，需改 PULLUP + IT_FALLING。
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin != KEY_UP_Pin) return;
    static uint32_t last_k = 0;
    uint32_t now = HAL_GetTick();
    if (now - last_k < 30) return;            // 30ms 内重复触发忽略 = 软件消抖
    last_k = now;
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
    printf("KEY pressed -> LED1 toggled\r\n");
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
