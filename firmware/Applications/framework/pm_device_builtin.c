#include "pm_device.h"

#include "drivers.h"   /* GC9D01 */
#include "algorithm/sw6306_algo.h"
#include "algorithm/sc7a20_algo.h"
#include "algorithm/sd3078_algo.h"
#include "FreeRTOS.h"
#include "task.h"
#include "wk_system.h"
#include "at32f423_wk_config.h"   /* wk_adc1_init()：唤醒后重新使能并校准 ADC1 */
#include "pm_api.h"           /* pm_api_lcd_blank_set()：显示输出门闩 */

/* AT32F423 MCU为一个PM设备：进低功耗是独立的“执行阶段”而非普通
 * suspend回调，以确保外围设备先挂起、RTC起点先记录、唤醒时钟先恢复。
 * Extra Low Power默认开启；需实机验证唤醒，不允许DeepSleep Debug。 */
#ifndef PM_AT32_EXTRA_LOW_POWER
#define PM_AT32_EXTRA_LOW_POWER 1
#endif

/* ── 睡眠期引脚处理（2026-10-27 实测确认的省电项，已转为产品行为）──
 *
 * 实测依据（7.85V 主控面板输入，差分固件 A/B）：
 *   - 睡眠期把 USART1 的 PA9/PA10 和 LCD 的 SPI/控制脚置于模拟输入后，
 *     面板电流明显下降；这些脚在睡眠期继续驱动会对外供电、或被板上钳位吸收。
 *   - 原固件睡眠期实测 TX 只有 1.25V（正常驱动应为 2.9V），说明线上确实有负载。
 * 因此：睡前把这些脚放成高阻，唤醒后**立刻**恢复成与 wk_*_init() 完全一致的配置。
 * 恢复必须在 xTaskResumeAll() 之前完成，否则唤醒后第一个 printf / 刷屏会发不出去。
 */
static void pm_sleep_pins_idle(void)
{
    gpio_init_type g;

    gpio_default_para_init(&g);
    g.gpio_mode = GPIO_MODE_ANALOG;
    g.gpio_pull = GPIO_PULL_NONE;

    /* USART1：TX(PA9) / RX(PA10) */
    g.gpio_pins = (uint16_t)(GPIO_PINS_9 | GPIO_PINS_10);
    gpio_init(GPIOA, &g);

    /* LCD：RST(PA8) / DC(PA12) / CS(PA15) —— 此时面板已 DISPOFF+SLPIN */
    g.gpio_pins = (uint16_t)(GPIO_PINS_8 | GPIO_PINS_12 | GPIO_PINS_15);
    gpio_init(GPIOA, &g);

    /* LCD：SCK(PB3) / MOSI(PB5) */
    g.gpio_pins = (uint16_t)(GPIO_PINS_3 | GPIO_PINS_5);
    gpio_init(GPIOB, &g);

    /* 引脚已全部高阻，最后才切 LCD 供电：这样不会有任何引脚继续反向给屏供电。
     * 断电后屏内寄存器全部丢失，唤醒必须走冷启动（pm_gc9d01_resume 已按此实现）。 */
    GC9D01_PowerRailSet(0);
}

static void pm_sleep_pins_restore(void)
{
    gpio_init_type g;

    /* 先给屏上电（内部含 5ms 稳定等待），再把 SPI/RST/DC 恢复成驱动模式：
     * 避免向未上电的屏灌电流，也保证复位/初始化命令发出时 VDD 已经稳。 */
    GC9D01_PowerRailSet(1);

    /* USART1：与 wk_usart1_init() 一致（MUX_7） */
    gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE9, GPIO_MUX_7);
    gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE10, GPIO_MUX_7);
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_mode = GPIO_MODE_MUX;
    g.gpio_pins = (uint16_t)(GPIO_PINS_9 | GPIO_PINS_10);
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOA, &g);

    /* LCD：SCK(PB3)/MOSI(PB5) 复用 SPI3，驱动强度与 wk_spi3_init() 一致 */
    gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE3, GPIO_MUX_6);
    gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE5, GPIO_MUX_6);
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_mode = GPIO_MODE_MUX;
    g.gpio_pins = (uint16_t)(GPIO_PINS_3 | GPIO_PINS_5);
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &g);

    /* LCD：CS(PA15) 复用 SPI3 硬件片选（MODERATE） */
    gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE15, GPIO_MUX_6);
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_mode = GPIO_MODE_MUX;
    g.gpio_pins = GPIO_PINS_15;
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOA, &g);

    /* LCD：RST(PA8) 输出高（不复位）、DC(PA12) 输出低，与 wk_gpio_config() 一致 */
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    g.gpio_pins = (uint16_t)(GPIO_PINS_8 | GPIO_PINS_12);
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_bits_set(GPIOA, GPIO_PINS_8);
    gpio_bits_reset(GPIOA, GPIO_PINS_12);
    gpio_init(GPIOA, &g);
}

static void pm_at32_deepsleep(void *ctx)
{
    (void)ctx;
    /* ③ 挂起 FreeRTOS 调度器：防止休眠过程中其他任务启动新的事务 */
    vTaskSuspendAll();

    /* ④ 忙等 10ms：确认无残留总线中断待处理（调度器已挂起，无新事务） */
    wk_delay_ms(10);

    /* ④b 日志排空后，把 UART/LCD 的驱动脚放成高阻（睡眠期不再对外驱动） */
    pm_sleep_pins_idle();

    /* ⑤ 停止 wk_tick（TMR7）时基：防止休眠过程中产生意外中断 */
    tmr_interrupt_enable(TMR7, TMR_OVF_INT, FALSE);
    tmr_counter_enable(TMR7, FALSE);
    NVIC_ClearPendingIRQ(TMR7_GLOBAL_IRQn);
    tmr_flag_clear(TMR7, TMR_OVF_FLAG);

    /* ⑥ 禁止 SysTick 中断并清 pending：防止 WFI 被时间片中断立即唤醒 */
    SysTick->CTRL &= ~(uint32_t)SysTick_CTRL_TICKINT_Msk;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

    /* ⑦ 关闭未用外设时钟：DeepSleep 期间 CPU 停，外设时钟仅增加静态功耗。
       保留 GPIOA/B/F（EXINT 唤醒需要）与 PWC/SCFG，唤醒后再恢复。 */
    /* ⑦ 睡前关闭片内 ADC1（实测最大单项：约 260µA 面板电流）
     * 为什么必须手动关：main() 里的 wk_adc1_init() 打开了 ADC1，并同时使能了
     * 「内部温度传感器 + VREFINT 缓冲」(ADCCOM.itsrven)。这两样在 DeepSleep 期间
     * 仍然耗电，而只关 ADC1 时钟是抓不到它们的。
     * 这里三步一起做：itsrven=0 → ADEN=0 → 关 ADC1 时钟（下面时钟列表）。
     * 唤醒侧见 pm_at32_deepsleep() 结尾的 wk_adc1_init()：重新使能并重新校准。 */
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);   /* 先开时钟，保证下面两笔寄存器写入生效 */
    {
        adc_common_config_type adc_common_off;

        adc_common_default_para_init(&adc_common_off);
        adc_common_off.div = ADC_HCLK_DIV_15;          /* 与 wk_adc1_init 的分频一致 */
        adc_common_off.tempervintrv_state = FALSE;     /* 关内部温度传感器/VREFINT 缓冲 */
        adc_common_config(&adc_common_off);
    }
    adc_enable(ADC1, FALSE);
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_CRC_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, FALSE);

    /* PB8共享IRQ可唤醒，按钮EXINT0/1/2维持启用。
     * 不要丢弃在总线排空/睡眠切换期间已经到达的真实IRQ事件。 */
    nvic_irq_enable(EXINT9_5_IRQn, 5, 0);

    /* ── 进入 DeepSleep ── */
    /* 按AT32参考时序先使用HICK，再调LDO、进入WFI。
     * 默认用已验证的LOW_POWER，1.0V EXTRA模式需要实机校验。 */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
    while (crm_flag_get(CRM_HICK_STABLE_FLAG) != SET);
    crm_sysclk_switch(CRM_SCLK_HICK);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_HICK);
#if PM_AT32_EXTRA_LOW_POWER
    PWC->ldoov_bit.ldoovsel = 0U; /* 1.0V，Extra Low Power专用 */
    pwc_voltage_regulate_set(PWC_REGULATOR_EXTRA_LOW_POWER);
#else
    pwc_voltage_regulate_set(PWC_REGULATOR_LOW_POWER);
#endif

    /* LICK 关闭（2026-10-27 产品化）：LICK 在 wk_periph_clock_config() 里打开后从没关过，
     * 而本工程未使用片内 ERTC / WWDT / 唤醒定时器，睡眠期用不到 32k 低速时钟。
     * 唤醒路径先开 HICK、不依赖 LICK，因此不需要恢复代码。 */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_LICK, FALSE);

    pwc_deep_sleep_mode_enter(PWC_DEEP_SLEEP_ENTER_WFI);

    /* ──── 唤醒后 ──── */

    /* ★ 清除 SLEEPDEEP 位：AT32 库函数未在唤醒后清除此位，
       否则下次空闲钩子执行 __WFI() 会再次进入 DeepSleep */
    SCB->SCR &= ~(uint32_t)SCB_SCR_SLEEPDEEP_Msk;

    /* ── 恢复 LDO 电压至 1.3V ──
     *
     * AT32F423 数据手册要求：
     *   LDO 1.0V → AHB ≤ 64 MHz
     *   LDO 1.2V → AHB ≤ 120 MHz
     *   LDO 1.3V → AHB ≤ 150 MHz
     *
     * DeepSleep 唤醒后 LDOOVSEL 回到复位值 0x2 (1.2V)，
     * 必须先在安全频率下切到 1.3V，再上 150MHz PLL，
     * 否则芯片工作可能不稳定甚至硬 fault。 */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
    while (crm_flag_get(CRM_HICK_STABLE_FLAG) != SET);
    crm_sysclk_switch(CRM_SCLK_HICK);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_HICK);
    pwc_ldo_output_voltage_set(PWC_LDO_OUTPUT_1V3); /* 先恢复1.3V */
    pwc_voltage_regulate_set(PWC_REGULATOR_ON);    /* 清VRSEL/VREXLPEN */
    flash_psr_set(FLASH_WAIT_CYCLE_4);

    /* ① 恢复系统时钟：DeepSleep 期间 PLL 及总线分频被硬件复位 */
    crm_pll_config(CRM_PLL_SOURCE_HICK, 75, 1, CRM_PLL_FR_2);   /* 150MHz: HICK×75/1/2 */
    crm_ahb_div_set(CRM_AHB_DIV_1);                             /* AHB = 系统时钟 /1 */
    crm_apb2_div_set(CRM_APB2_DIV_2);                           /* APB2 = AHB /2 */
    crm_apb1_div_set(CRM_APB1_DIV_2);                           /* APB1 = AHB /2 */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);         /* 使能 PLL */
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET);           /* 等待 PLL 锁定 */
    crm_auto_step_mode_enable(TRUE); /* 150MHz >108MHz，与上电初始化相同 */
    crm_sysclk_switch(CRM_SCLK_PLL);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL);
    crm_auto_step_mode_enable(FALSE);
    system_core_clock_update();                                   /* 更新全局时钟变量 */


    /* 直到外设时钟恢复完成前，FreeRTOS保持暂停。 */

    /* ⑤ 恢复 wk_tick（TMR7）时基 */
    tmr_counter_enable(TMR7, FALSE);
    tmr_flag_clear(TMR7, TMR_OVF_FLAG);
    tmr_counter_enable(TMR7, TRUE);
    tmr_interrupt_enable(TMR7, TMR_OVF_INT, TRUE);

    /* ⑦ 恢复外设时钟（与休眠前关闭的逐一对应） */
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
    /* ADC1由load_task按需启用，不在唤醒时恢复常开。 */
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_CRC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, TRUE);

    /* 时钟树和外设均已恢复，先把 UART/LCD 引脚恢复成产品配置（LCD VDD 也在这里上电），
     * 再开启 tick 与 RTOS 调度——否则唤醒后第一句 printf / 第一次刷屏会发不出去。 */
    pm_sleep_pins_restore();

    /* 不在PM恢复时重新开启ADC1：只有Status/System页采样请求会在
     * load_task上电并校准，采样结束立即关闭。 */

    SysTick->CTRL |= (uint32_t)SysTick_CTRL_TICKINT_Msk;
    xTaskResumeAll();


}

static void pm_at32_standby(void *ctx)
{
    gpio_init_type gpio_init_struct;
    (void)ctx;
    /* WLED硬件直接归零。PB0保持低电平输出且无上下拉，避免PWM输入悬空；
     * PB1(CONF)不再作为唤醒源，切到Analog/no-pull降低输入与上拉静态电流。 */
    WLED_SetPwm(0U);
    gpio_bits_reset(GPIOB, GPIO_PINS_0);
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_pins = GPIO_PINS_0;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_init_struct);

    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
    gpio_init_struct.gpio_pins = GPIO_PINS_1;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_init_struct);

    /* Standby同样隔离LCD控制引脚并主动关闭PA3~PA7供电；已由suspend完成SLPIN。 */
    pm_sleep_pins_idle();

    /* 唯一Standby唤醒源：PA0 = NEXT/PWR = WKUP1，按键拉高唤醒。
     * 显式关闭其它WKUP，避免外部事件把运输模式误唤醒。 */
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
    gpio_init_struct.gpio_pins = GPIO_PINS_0;
    gpio_init_struct.gpio_pull = GPIO_PULL_DOWN;
    gpio_init(GPIOA, &gpio_init_struct);

    pwc_wakeup_pin_enable(PWC_WAKEUP_PIN_2, FALSE);
    pwc_wakeup_pin_enable(PWC_WAKEUP_PIN_6, FALSE);
    pwc_wakeup_pin_enable(PWC_WAKEUP_PIN_7, FALSE);
    pwc_flag_clear(PWC_WAKEUP_FLAG | PWC_STANDBY_FLAG);
    pwc_wakeup_pin_enable(PWC_WAKEUP_PIN_1, TRUE);
    pwc_standby_mode_enter();
}


static void pm_gc9d01_prepare(void *ctx)
{
    (void)ctx;
    GC9D01_SetBL(0);
}

static void pm_gc9d01_suspend(void *ctx)
{
    (void)ctx;
    GC9D01_DisplayPower(0);
    /* 面板已 DISPOFF：置"显示输出待恢复"门闩，唤醒后由 ui_task 在首帧写完后
     * 发 DISPON 再点亮背光（不依赖 ui_task 的某个分支何时执行）。
     * 真正的 VDD 切断在 pm_sleep_pins_idle() 末尾（引脚高阻之后）。 */
    pm_api_lcd_blank_set(1U);
    GC9D01_SleepModeEnter();
}

static void pm_gc9d01_resume(void *ctx)
{
    (void)ctx;

    /* 睡眠期 LCD VDD 已断开，屏内寄存器全部丢失 → 唤醒必须走完整冷启动：
     * 复位 + 全部寄存器配置（二维表）+ 关背光。DISPON 交给 ui_task：
     * 它把首帧写进 GRAM 之后再发 0x29，面板锁存到的第一帧就是新画面。 */
    GC9D01_Init(); /* Init内部已经执行RESET */
    GC9D01_SetBL(0);
}

/* SW6306 / SC7A20 的具体低功耗策略已移入各自 algorithm。
 * framework这里只保留设备注册顺序，避免跨层持有外设业务判断。 */

static const pm_device_node_t s_pm_gc9d01 = {
    "gc9d01",
    pm_gc9d01_prepare,
    pm_gc9d01_suspend,
    pm_gc9d01_resume,
    0,
    10,
    0,
    0
};

static const pm_device_node_t s_pm_sc7a20 = {
    "sc7a20",
    0,
    SC7A20_AlgoPmSuspend,
    SC7A20_AlgoPmResume,
    0,
    15,
    0,
    0
};

static const pm_device_node_t s_pm_sw6306 = {
    "sw6306",
    0,
    SW6306_AlgoPmSuspend,
    SW6306_AlgoPmResume,
    0,
    20,
    0,
    0
};

static const pm_device_node_t s_pm_sd3078 = {
    "sd3078",
    0,
    SD3078_AlgoPmSuspend,
    0,
    0,
    25,
    0,
    0
};

static const pm_device_node_t s_pm_at32 = {
    "at32f423",
    0, 0, 0, 0, 30,
    pm_at32_deepsleep,
    pm_at32_standby
};

void pm_device_register_builtin(void)
{
    pm_device_register(&s_pm_gc9d01);
    pm_device_register(&s_pm_sc7a20);
    pm_device_register(&s_pm_sw6306);
    pm_device_register(&s_pm_sd3078);
    pm_device_register(&s_pm_at32);
}
