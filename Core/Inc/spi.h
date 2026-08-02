#ifndef SPI_H
#define SPI_H

#include <stdint.h>

/* SPI1 引脚（F407ZGT6 默认复用 AF5）
 *   PA5 = SCK,  PA7 = MOSI,  PA6 = MISO,  PA4 = CS(NSS)
 * 回环测试：用杜邦线把 PA7(MOSI) 与 PA6(MISO) 短接即可自己发自己收。
 */
void    SPI1_Init(void);
uint8_t SPI1_TransmitReceive(uint8_t tx);
int     SPI1_LoopbackTest(void);   /* 返回 1=全部通过, 0=有错 */

#endif /* SPI_H */
