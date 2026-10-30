#ifndef MCU_ADC_H
#define MCU_ADC_H
#include <stdint.h>
/* load_task独占ADC1；ui_task仅设置请求，菜单读取最近一次RAM结果。 */
void mcu_adc_set_active(uint8_t active);
uint8_t mcu_adc_is_busy(void); /* PM检查；只在ADCEN/ITSRVEN确实关闭后归零 */
uint8_t mcu_adc_read_cached(uint16_t *vcc_mv, int16_t *temp_dC);
#endif
