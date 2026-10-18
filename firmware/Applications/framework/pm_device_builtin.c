#include "pm_device.h"

#include "drivers.h"   /* GC9D01 */
#include "algorithm/sw6306_algo.h"
#include "algorithm/sc7a20_algo.h"
#include "algorithm/sd3078_algo.h"
#include "FreeRTOS.h"
#include "task.h"
#include "wk_system.h"

/* AT32F423 MCU为一个PM设备：进低功耗是独立的“执行阶段”而非普通
 * suspend回调，以确保外围设备先挂起、RTC起点先记录、唤醒时钟先恢复。
 * 额外低功耗LDO需在量产板验证唤醒时序后再开启。 */
#ifndef PM_AT32_EXTRA_LOW_POWER
#define PM_AT32_EXTRA_LOW_POWER 0
#endif

static void pm_at32_deepsleep(void *ctx)
{
    (void)ctx;
    /* ③ 挂起 FreeRTOS 调度器：防止休眠过程中其他任务启动新的事务 */
    vTaskSuspendAll();

    /* ④ 忙等 10ms：确认无残留总线中断待处理（调度器已挂起，无新事务） */
    wk_delay_ms(10);

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
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_CRC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, TRUE);

    /* 时钟树和外设均已恢复，最后开启tick与RTOS调度。 */
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
    GC9D01_SleepModeEnter();
}

static void pm_gc9d01_resume(void *ctx)
{
    (void)ctx;
    GC9D01_SleepModeExit();
    GC9D01_DisplayPower(1);
    /* 保持灭：亮屏由 ui_task 在唤醒数据预取完成、绘出新图后按菜单背光值恢复
     * （避免休眠前旧值闪现）。 */
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
