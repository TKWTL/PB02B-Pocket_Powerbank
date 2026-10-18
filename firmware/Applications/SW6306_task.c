#include "applications.h"
#include "freertos_app.h"

/* UI心跳只增计数、写phase；此处仅在已有2.5s串口状态周期读取。 */
extern volatile uint32_t g_ui_heartbeat;
extern volatile uint8_t g_ui_phase;
extern volatile uint8_t g_gspi_phase;

static void sw6306_update_sleep_block(void)
{
    uint8_t port_on;
    uint8_t bus_active;

    /* A low-current idle load must not put the MCU/SW6306 to sleep. */
    pm_api_set_sleep_block(PM_BLOCK_LOW_CURRENT,
        SW6306_AlgoGetSpecialMode() == SW6306_SPECIAL_LOW_CURRENT);

    /* 任意端口通路打开（本硬件真实口：C1/A1；A1 兼作 WLED 假插入口。
     * 假插入与真实负载由 UI 用 BUS 电流区分，见 main_screen.c a1_is_real_load） */
    port_on = (SW6306_IsPortC1ON() != 0 || SW6306_IsPortA1ON() != 0) ? 1 : 0;
    /* 总线有压且有电流 = 充电/放电活跃，不依赖端口 ON 位（防端口位漏检误入睡） */
    bus_active = (SW6306_ReadVBUS() > 4000 && SW6306_ReadIBUS() > 50) ? 1 : 0;

    /* 端口一旦打开就持续保持RUN，避免无负载30s休眠时LPSet改变通路。
     * 空载由用户PWR长按主动断开；没有输出口时仍允许30s后睡眠。 */
    if (port_on != 0 || bus_active != 0) {
        pm_api_set_sleep_block(PM_BLOCK_SW6306_LOAD, 1);
        pm_api_refresh_idle();
    } else {
        pm_api_set_sleep_block(PM_BLOCK_SW6306_LOAD, 0);
    }
}

/* 睡眠门控检查：置位则让出（停止 I2C 读写），返回 1=被门控 */
static uint8_t sw6306_gate_check(void)
{
    if (pm_api_sleep_gate_get() != 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return 1;
    }
    return 0;
}

void SW6306_task_func(void *pvParameters)
{
    (void)pvParameters;
    uint8_t dbg_idx = 0U;
    uint8_t service_div = 0U;

    /* 算法层持有Load相位、容量1s分频和业务状态；任务只提供100ms调度节拍。 */
    SW6306_AlgoInit();

    while (1) {
        if (sw6306_gate_check()) continue;
        vTaskDelay(pdMS_TO_TICKS(SW6306_ALGO_LOAD_STEP_MS));
        if (sw6306_gate_check()) continue;

        SW6306_AlgoLoadStep();
        SW6306_AlgoService100ms();
        SW6306_AlgoProcessCommands();

        if (SW6306_IsInitialized() == 0U) {
            USART_Printf("[SW6306] Re-Inited.\n");
            SW6306_AlgoInvalidateDischargeSession();
            SW6306_ForceOff();
            SW6306_Init();
            /* 关闭外部系统电流补算（0xA4 EXTSYS 区）。
             * 那是旧版硬件的方案：控制电路（含 WLED Boost）从电池端检流电阻“之后”取电，
             * 检流电阻读不到这部分电流，只能由 MCU 写寄存器补算，
             * 所以旧代码在这里把方向设成放电、并塞一个固定 10mA。
             * 新版硬件取电点统一移到检流电阻“之前”，WLED 电流天然流经检流电阻、
             * 由芯片内部库仑计统计（实测：A1 假插入亮灯时 BatCap 会下降）。
             * 因此这里必须保持关闭，否则同一份电流被重复计入两次。
             * SW6306_Iext* 三个 API 保留在驱动里（SW6306 库是面向通用驱动的），仅本应用不再使用。 */
            SW6306_IextEnSet(0);
            if (SW6306_IsInitialized()) {
                SW6306_AlgoOnDriverReinitialized();
            }
        }

        /* 保持原先约500ms的睡眠判定/调试输出节拍。 */
        if (++service_div < 5U) continue;
        service_div = 0U;

        sw6306_update_sleep_block();

        if (dbg_idx == 0U) {
            uint32_t mw = ((uint32_t)SW6306_ReadVBUS() * SW6306_ReadIBUS() + 500U) / 1000U;
            USART_Printf("VBUS:%dmV\tIBUS:%dmA\tPBUS:%lu.%03luW\n",
                         SW6306_ReadVBUS(), SW6306_ReadIBUS(),
                         (unsigned long)(mw / 1000U), (unsigned long)(mw % 1000U));
        } else if (dbg_idx == 1U) {
            uint32_t mw = ((uint32_t)SW6306_ReadVBAT() * SW6306_ReadIBAT() + 500U) / 1000U;
            USART_Printf("VBAT:%dmV\tIBAT:%dmA\tPBAT:%lu.%03luW\n",
                         SW6306_ReadVBAT(), SW6306_ReadIBAT(),
                         (unsigned long)(mw / 1000U), (unsigned long)(mw % 1000U));
        } else if (dbg_idx == 2U) {
            /* 借助仍运行的SW6306任务定位卡屏：状态/栈/PM就绪情况。 */
            USART_Printf("[UID2] h:%lu p:%u spi:%u st:%u hw:%lu\n",
                         (unsigned long)g_ui_heartbeat, (unsigned)g_ui_phase,
                         (unsigned)g_gspi_phase, (unsigned)eTaskGetState(ui_task_handle),
                         (unsigned long)uxTaskGetStackHighWaterMark(ui_task_handle));
            USART_Printf("[UID2] b:%u pm:%u rd:%u mask:%02X\n",
                         (unsigned)eTaskGetState(button_task_handle),
                         (unsigned)pm_api_ui_should_block(),
                         (unsigned)pm_api_wake_data_ready(),
                         (unsigned)pm_api_get_block_mask());
            float temp = SW6306_ReadTCHIP();
            int32_t t10 = (int32_t)(temp * 10.0f + ((temp >= 0.0f) ? 0.5f : -0.5f));
            uint32_t mag = (uint32_t)((t10 < 0) ? -t10 : t10);
            USART_Printf("TChip:%s%lu.%luC\tBatCap:%d%%\tSleep:%lums\n\n",
                         (t10 < 0) ? "-" : "",
                         (unsigned long)(mag / 10U), (unsigned long)(mag % 10U),
                         SW6306_ReadCapacity(), pm_sleep_timer_left_ms());
        }

        dbg_idx++;
        if (dbg_idx >= 5U) dbg_idx = 0U;
    }
}
