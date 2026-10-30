/* load_task.c - 慢速外设/系统服务任务（10ms 状态机）
 *
 * 单线程负责 SD3078、SC7A20、NVM 的实际 I2C 提交：
 *  - 10ms：处理 RTC/后备电池 pending（无请求时不访问I2C）；
 *  - 200ms：普通SC7A20 5Hz；水平仪运行40ms LOAD/50Hz传感器；
 *  - 500ms：SD3078 Time Load、NVM dirty 合并写、WLED 慢速守护；
 *  - 60s：SD3078 Temp/VBAT Load + MS621FE Auto充电策略复核。
 * 各外设具体Load顺序/分频放在对应algo；task只提供调度节拍。
 */
#include "applications.h"
#include "framework/mcu_adc.h"
#include "at32f423_wk_config.h"

/* 只由load_task访问ADC1；菜单/UI仅通过RAM镜像请求/读取。
 * 内部基准典型1.20V，温度25°C典型1.29V/斜率-4.26mV/°C。
 * VDDA与VCC同轨时近似显示VCC，绝对温度需单点校准。 */
static volatile uint8_t s_mcu_adc_active;
static volatile uint8_t s_mcu_adc_valid;
static volatile uint16_t s_mcu_adc_vcc_mv;
static volatile int16_t s_mcu_adc_temp_dC;
static uint32_t s_mcu_adc_last_tick;

void mcu_adc_set_active(uint8_t active)
{
    uint8_t on = active ? 1U : 0U;
    taskENTER_CRITICAL();
    if (on && !s_mcu_adc_active) {
        s_mcu_adc_last_tick = 0U; /* 首次进入信息页立即采样 */
        s_mcu_adc_valid = 0U;
    }
    s_mcu_adc_active = on;
    taskEXIT_CRITICAL();
}

uint8_t mcu_adc_read_cached(uint16_t *vcc_mv, int16_t *temp_dC)
{
    uint8_t valid;
    taskENTER_CRITICAL();
    valid = s_mcu_adc_valid;
    if (vcc_mv) *vcc_mv = s_mcu_adc_vcc_mv;
    if (temp_dC) *temp_dC = s_mcu_adc_temp_dC;
    taskEXIT_CRITICAL();
    return valid;
}

static uint8_t mcu_adc_wait_ready(void)
{
    uint32_t timeout = 100000U;
    while (adc_flag_get(ADC1, ADC_RDY_FLAG) == RESET)
        if (--timeout == 0U) return 0U;
    return 1U;
}

static uint8_t mcu_adc_read_one(adc_channel_select_type channel, uint16_t *raw)
{
    uint32_t timeout = 100000U;
    adc_ordinary_channel_set(ADC1, channel, 1U, ADC_SAMPLETIME_92_5);
    adc_flag_clear(ADC1, ADC_OCCE_FLAG);
    adc_ordinary_software_trigger_enable(ADC1, TRUE);
    while (adc_flag_get(ADC1, ADC_OCCE_FLAG) == RESET)
        if (--timeout == 0U) return 0U;
    *raw = adc_ordinary_conversion_data_get(ADC1);
    return 1U;
}

/* 单次上电→校准→两通道顺序读取→关闭模拟电路与ADC1时钟。
 * 包括失败分支的ADCEN/ITSRVEN关断，保证DeepSleep不多耗电。 */
static uint8_t mcu_adc_sample_once(uint16_t *vcc_mv, int16_t *temp_dC)
{
    adc_common_config_type common;
    adc_base_config_type base;
    uint16_t ref_raw, temp_raw;
    uint32_t limit;
    uint8_t ok = 0U;
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
    adc_reset();
    crm_adc_clock_select(CRM_ADC_CLOCK_SOURCE_HCLK);
    adc_common_default_para_init(&common);
    common.div = ADC_HCLK_DIV_15;
    common.tempervintrv_state = TRUE;
    adc_common_config(&common);
    adc_base_default_para_init(&base);
    base.sequence_mode = FALSE;
    base.repeat_mode = FALSE;
    base.data_align = ADC_RIGHT_ALIGNMENT;
    base.ordinary_channel_length = 1U;
    adc_base_config(ADC1, &base);
    adc_resolution_set(ADC1, ADC_RESOLUTION_12B);
    adc_ordinary_conversion_trigger_set(ADC1, ADC_ORDINARY_TRIG_SOFTWARE,
                                        ADC_ORDINARY_TRIG_EDGE_NONE);
    adc_enable(ADC1, TRUE);

    /* 温度传感器启动最长100us；2个tick避免不足。 */
    vTaskDelay(pdMS_TO_TICKS(2) + 1U);
    if (!mcu_adc_wait_ready()) goto cleanup;
    adc_calibration_init(ADC1);
    limit = 100000U;
    while (adc_calibration_init_status_get(ADC1))
        if (--limit == 0U) goto cleanup;
    adc_calibration_start(ADC1);
    limit = 100000U;
    while (adc_calibration_status_get(ADC1))
        if (--limit == 0U) goto cleanup;
    if (!mcu_adc_read_one(ADC_CHANNEL_17, &ref_raw) || ref_raw < 100U) goto cleanup;
    if (!mcu_adc_read_one(ADC_CHANNEL_16, &temp_raw)) goto cleanup;

    {
        float vd = (4095.0f * 1200.0f) / (float)ref_raw;
        float vts = ((float)temp_raw * vd) / 4095.0f;
        float temp = (25.0f + (1290.0f - vts) / -4.26f) * 10.0f;
        if (vd < 1600.0f || vd > 3600.0f ||
            temp < -1000.0f || temp > 1500.0f) goto cleanup;
        *vcc_mv = (uint16_t)(vd + 0.5f);
        *temp_dC = (int16_t)(temp + (temp >= 0 ? 0.5f : -0.5f));
        ok = 1U;
    }
cleanup:
    adc_enable(ADC1, FALSE);
    common.tempervintrv_state = FALSE;
    adc_common_config(&common);
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, FALSE);
    return ok;
}


/* 延迟复位（实现在 functions/reset.c，声明在 functions/functions.h）。
 * 这里只做前置声明，不包含 functions.h：后者为 UI 头（lvgl.h + menu.h），
 * 而本文件属于 Tasks 组、不应耦合 UI 层。
 * 调用语义：负载任务每 500ms 调用一次；函数内部先判 pending 与 NVM 干净，
 * 仅当 UI 侧 action_reset_now() 置过 pending 时才真正 nvic_system_reset()，
 * 因此周期调用是幂等且安全的。 */
void action_reset_process(void);

/* SD3078 不再维护 initialized flag；应用层只根据每次 API 的 i2c_status_type
 * 决定是否继续使用/重试初始化。 */
static i2c_status_type s_sd3078_status = I2C_ERR_INTERRUPT;
static uint8_t s_nvm_ready;

static void sd3078_try_init(void)
{
    i2c_status_type st;

    if (s_sd3078_status == I2C_OK) return;

    s_sd3078_status = SD3078_Init();
    if (s_sd3078_status != I2C_OK) return;

    /* NVM 依赖 SD3078 Backup RAM，因此只在 RTC 驱动已可靠在线后初始化。 */
    if (!s_nvm_ready) {
        st = nvm_init();
        if (st != I2C_OK) {
            s_sd3078_status = st;
            return;
        }
        s_nvm_ready = 1U;

        /* 持久化校准值在业务算法开始前装入各自驱动。 */
        SW6306_SetBattRShunt(nvm_get_shunt_mohm());
        SC7A20_SetXCalibration(nvm_get_sc7a20_x_zero_raw(),
                               nvm_get_sc7a20_x_slope_mg_per_lsb());
        SC7A20_SetYCalibration(nvm_get_sc7a20_y_zero_raw(),
                               nvm_get_sc7a20_y_slope_mg_per_lsb());
        SC7A20_SetZCalibration(nvm_get_sc7a20_z_zero_raw(),
                               nvm_get_sc7a20_z_slope_mg_per_lsb());
    }

    /* SD3078_Init会把充电寄存器恢复为关闭；algo重新装载持久化模式。
     * 初始化后立即走一次全量Load，保证Auto充电第一次判定就使用fresh Temp/VBAT。 */
    SD3078_AlgoInit();
    s_sd3078_status = SD3078_AlgoLoadAll();
}

/* 一轮完整读取（唤醒预取 / 事件即时刷新）：SD3078 + SC7A20 + SW6306 镜像。
 * 读取顺序：先读 SD3078/SC7A20（唤醒后立即就绪），SW6306 放最后——
 * 其 I2C 操作天然为 SW6306 从 LPSet 唤醒留出就绪时间；唤醒预取时 SW6306 前再
 * 加稳定延迟，避免读到 ADC 未更新的无效值（VBUS/IBAT 等相邻通道 raw 相同）。 */
static void data_refresh_all(void)
{
    sd3078_try_init();
    if (s_sd3078_status == I2C_OK) {
        s_sd3078_status = SD3078_AlgoLoadAll();
        if (s_sd3078_status == I2C_OK) {
            s_sd3078_status = SD3078_AlgoProcessFast();
        }
    }

    SC7A20_AlgoLoadSample();
    if (SC7A20_IsInitialized()) SC7A20_TempLoad();

    if (SW6306_IsInitialized()) {
        /* SW6306刚从LPSet唤醒时ADC需要稳定时间；任务只负责时序，
         * 真正的寄存器Load顺序由SW6306 algo持有。 */
        if (pm_api_wake_data_ready() == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        SW6306_AlgoLoadAll();
    }
}

/* ==================== WLED algorithm executor ====================
 * wled_algo owns brightness/thermal/ramp/fake-A1 state. This task only
 * advances it and performs the requested hardware operations. */
static void wled_service_10ms(void)
{
    uint16_t pwm;
    uint16_t pwm_out;
    wled_port_cmd_t cmd;

    WLED_AlgoTick10ms();

    cmd = WLED_AlgoTakePortCommand();
    if (cmd == WLED_PORT_CMD_A1_INSERT) {
        SW6306_PortA1Insert();
    } else if (cmd == WLED_PORT_CMD_A1_REMOVE) {
        SW6306_PortA1Remove();
    }

    if (WLED_AlgoTakePwm(&pwm) != 0U) {
        WLED_SetPwm(pwm);
    }

    /* 休眠门控绑定【实际 PWM 输出】，而不是 s_level（用户意图）。
     * 原因：s_level 在收到开关请求时立刻置位，但 PWM 还要过计量通路门控
     * （s_path_ok）和 160ms 渐变才会真正输出。若假插入口没确认（s_path_ok=0），
     * s_level 已是 12 而 s_pwm 恒为 0 —— 此时若按 s_level 阻止休眠，
     * 就会出现"灯没亮、却一直拒绝休眠、30s 倒计时被打断"的假激活状态。 */
    pwm_out = WLED_AlgoGetPwm();
    pm_api_set_sleep_block(PM_BLOCK_WLED, (pwm_out != 0U) ? 1U : 0U);
    if (pwm_out != 0U) pm_api_refresh_idle();
}

/* load_task 任务入口：10ms 状态机（周期 500ms 刷新 + 请求即立即一轮完整读取）
 * 注：曾尝试"INT 刷新提醒驱动"（SD3078 在共用 INT 线上输出 2Hz，由 EXTI 通知本任务），
 * 因长期占用共用 INT 线已回滚，恢复为周期轮询 + 事件驱动。 */
void load_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t period_cnt = 0U;
    uint8_t sc7_cnt = 0U;
    uint8_t sd3078_slow_cnt = 0U;
    uint16_t sc7_temp_ticks = 0U;
    uint8_t sc7_configured = 0xFFU;

    WLED_Init();
    WLED_AlgoInit();
    SC7A20_AlgoInit();

    for (;;) {
        /* 睡眠准备/DeepSleep期间停止发起新的总线事务。 */
        if (pm_api_sleep_gate_get() != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 仅Status/System页请求，每秒最多一次；测量完成立刻掉电。 */
        if (s_mcu_adc_active &&
            (s_mcu_adc_last_tick == 0U ||
             (uint32_t)(xTaskGetTickCount() - s_mcu_adc_last_tick) >= pdMS_TO_TICKS(1000))) {
            uint16_t vcc;
            int16_t temp;
            s_mcu_adc_last_tick = xTaskGetTickCount();
            if (mcu_adc_sample_once(&vcc, &temp)) {
                taskENTER_CRITICAL();
                s_mcu_adc_vcc_mv = vcc;
                s_mcu_adc_temp_dC = temp;
                s_mcu_adc_valid = 1U;
                taskEXIT_CRITICAL();
            } else {
                s_mcu_adc_valid = 0U;
            }
        }

        /* SD3078 快速服务：没有 pending 时只检查 RAM flag，不访问 I2C。 */
        if (s_sd3078_status == I2C_OK) {
            i2c_status_type st = SD3078_AlgoProcessFast();
            if (st != I2C_OK) s_sd3078_status = st;
        }

        /* 只有load_task配置运行ODR，UI不碰I2C；DeepSleep挂起时本段不执行。
         * PM恢复先按当前水平仪标志恢复50Hz，退出水平仪后这里退回25Hz。 */
        {
            uint8_t active = SC7A20_AlgoLevelerActive();
            uint8_t ticks = active ? 4U : (SC7A20_ALGO_SAMPLE_MS / 10U);
            if (SC7A20_IsInitialized() && sc7_configured != active) {
                SC7A20_SetODR(active ? SC7A20_ODR_50HZ : SC7A20_DEFAULT_ODR);
                sc7_configured = active;
                sc7_cnt = 0U;
            }
            if (++sc7_cnt >= ticks) {
                sc7_cnt = 0U;
                SC7A20_AlgoLoadSample();
                /* Temp继续0.5Hz，避免水平仪模式额外高频读温度。 */
                sc7_temp_ticks += ticks;
                if (sc7_temp_ticks >= 200U) {
                    sc7_temp_ticks = 0U;
                    if (SC7A20_IsInitialized()) SC7A20_TempLoad();
                }
            }
        }

        if (pm_api_data_refresh_pending() != 0) {
            data_refresh_all();
            pm_api_data_refresh_done();
        } else if (++period_cnt >= 50U) {  /* 50×10ms = 500ms */
            period_cnt = 0U;

            sd3078_try_init();

            if (!SC7A20_IsInitialized()) {
                SC7A20_Init();       /* 初始化默认25Hz；下一轮按水平仪需求切50Hz */
                sc7_configured = 0xFFU;
                SC7A20_AlgoInit();
                exint_flag_clear(EXINT_LINE_8);
                NVIC_ClearPendingIRQ(EXINT9_5_IRQn);
            }

            if (s_sd3078_status == I2C_OK) {
                /* Time保持500ms；Temp/VBAT由Slow入口降到60s。 */
                s_sd3078_status = SD3078_AlgoLoadFast();

                /* NVM setter只置dirty；500ms合并窗口避免菜单连续步进造成密集SRAM写。 */
                if (s_sd3078_status == I2C_OK && s_nvm_ready && nvm_is_dirty()) {
                    s_sd3078_status = nvm_process();
                }
                if (s_sd3078_status == I2C_OK && s_nvm_ready) {
                    action_reset_process();
                }

                if (s_sd3078_status == I2C_OK && ++sd3078_slow_cnt >= 120U) {
                    sd3078_slow_cnt = 0U;
                    s_sd3078_status = SD3078_AlgoLoadSlow();
                }
            }

            WLED_AlgoUpdate500ms();
        }

        wled_service_10ms();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
