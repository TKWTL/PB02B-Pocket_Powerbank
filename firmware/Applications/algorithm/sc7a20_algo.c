/*
 * sc7a20_algo.c - low-rate 180-degree orientation estimator
 */
#include "sc7a20_algo.h"
#include "sc7a20.h"
#include "framework/pm_api.h"
#include "FreeRTOS.h"
#include "task.h"
#include "at32f423.h"  /* shared SC7A20/SW6306 IRQ input PB8 */

sc7a20_algo_config_t SC7A20_AlgoConfig = {
    0U, /* pickup_wake：默认关闭，保持现有最低休眠功耗 */
    0U  /* motion_wake：默认关闭 */
};

/************************* SC7A20 180°姿态判断算法开始 **************************/
/* 应用层以 5Hz 读取校准后的 X 轴：
 * X≈-1g 判为 NORMAL，X≈+1g 判为 FLIPPED，中间区域不累计；
 * 新方向需连续约2s满足阈值才确认，避免手持抖动导致屏幕反复翻转。 */

static volatile sc7a20_orientation_t s_orientation;
static sc7a20_orientation_t s_candidate;
static uint8_t s_candidate_count;
void SC7A20_AlgoInit(void)
{
    /* 也会在重力校准完成时由UI线程调用，因此与5Hz load_task 更新互斥。 */
    taskENTER_CRITICAL();
    s_orientation = SC7A20_ORIENT_UNKNOWN;
    s_candidate = SC7A20_ORIENT_UNKNOWN;
    s_candidate_count = 0U;
    taskEXIT_CRITICAL();
}

void SC7A20_AlgoUpdate(float x_mg)
{
    sc7a20_orientation_t next;

    /* 本机安装方向：X≈-1g为正常，X≈+1g为倒置；中间区域不推进计时。 */
    if (x_mg <= -SC7A20_ALGO_ENTER_MG) {
        next = SC7A20_ORIENT_NORMAL;
    } else if (x_mg >= SC7A20_ALGO_ENTER_MG) {
        next = SC7A20_ORIENT_FLIPPED;
    } else {
        s_candidate = SC7A20_ORIENT_UNKNOWN;
        s_candidate_count = 0U;
        return;
    }

    if (next == s_orientation) {
        s_candidate = SC7A20_ORIENT_UNKNOWN;
        s_candidate_count = 0U;
        return;
    }

    if (next != s_candidate) {
        s_candidate = next;
        s_candidate_count = 0U;
        return;
    }

    if (++s_candidate_count >= SC7A20_ALGO_CONFIRM_SAMPLES) {
        s_orientation = next;
        s_candidate = SC7A20_ORIENT_UNKNOWN;
        s_candidate_count = 0U;
    }
}

sc7a20_orientation_t SC7A20_AlgoGetOrientation(void)
{
    return s_orientation;
}

void SC7A20_AlgoLoadSample(void)
{
    if (!SC7A20_IsInitialized()) return;

    SC7A20_AccelLoad();
    if (SC7A20_IsInitialized()) {
        SC7A20_AlgoUpdate(SC7A20_ReadX_mg());
    }
}
/************************* SC7A20 180°姿态判断算法结束 **************************/

/**************** SC7A20 DeepSleep hardware wake monitor *****************
 * 致谢：大飞《摇一摇换一签：SC7A20 运动中断调教记录》
 * https://shenyifei.com/posts/sc7a20-motion-irq
 * 其 AOI 高通去重力、LIR 锁存与读 SRC 释放的实践为本项目调试提供了参考。
 * 本项目另采用 25Hz、AOI2/6D 和共用 PB8 的休眠唤醒策略。
 * Shared PB8/EXTI8 is low-active; ISR always wakes without classifying source.
 * Motion: AOI1 (0x30) XYZ OR + high-pass (CTRL2.HPIS1), INT1 CTRL3[6].
 * Pickup: AOI2 (0x34) 6D Movement XYZ axes, INT1 CTRL3[5].
 * 两路共享 25Hz/±2g 低功耗采样；AOI1 采用高通并在布防前读 REFERENCE 清除重力。
 * Motion 仅使用 XYZ 正向事件，增加阈值与持续时间以避免静置误报；
 * 只锁存 AOI1，Pickup/AOI2 保持原非锁存方式。
 * 唤醒后先读 SOURCE 释放锁存，再撤销路由，避免 PB8 长期被拉低。
 */
static void sc7a20_wake_routes_off(void)
{
    /* Mask IRQ routes before reprogramming threshold/modes. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL3,
        SC7A20_CTRL3_I1_AOI1 | SC7A20_CTRL3_I1_AOI2, 0U);
    /* Clear AOI1 after each wake; next suspend re-arms after HPF settles. */
    SC7A20_ByteWrite(SC7A20_CTRG_INT1_CFG, 0U);
    SC7A20_ByteWrite(SC7A20_CTRG_INT2_CFG, 0U);
    /* 清除休眠用的高通路由及锁存状态，避免影响正常运行。 */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL2,
        SC7A20_CTRL2_HPM_MSK | SC7A20_CTRL2_HPCF_MSK |
        SC7A20_CTRL2_HPIS1 | SC7A20_CTRL2_HPIS2, 0U);
    SC7A20_ByteModify(SC7A20_CTRG_CTRL5,
        SC7A20_CTRL5_LIR_INT1 | SC7A20_CTRL5_LIR_INT2, 0U);
}

void SC7A20_AlgoPmSuspend(void *ctx)
{
    uint8_t routes = 0U;
    volatile uint8_t scratch;
    (void)ctx;

    /* 首次直接休眠/运输时也须先设置共享 PB8 极性，再进入 Power-down。 */
    if (!SC7A20_IsInitialized()) SC7A20_Init();
    if (!SC7A20_IsInitialized()) return;
    sc7a20_wake_routes_off();
    /* No temperature ADC needed while the CPU is asleep.
     * TEMP_CFG bit semantics follow LIS2DH compatibility, unverified. */
    SC7A20_ByteWrite(SC7A20_CTRG_TEMP_CFG, 0U);

    if (pm_api_transport_mode_requested() ||
        (!SC7A20_AlgoConfig.pickup_wake && !SC7A20_AlgoConfig.motion_wake)) {
        SC7A20_LowPowerSet(); /* 无唤醒或运输模式：ODR=0，真正 Power-down */
        return;
    }

    /* PB8 低有效；仅 Motion/AOI1 锁存，Pickup/AOI2 仍按原配置运行。
     * 守护逻辑会在持续低电平时读 SOURCE 清锁存，必要时撤销唤醒路由。 */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL6,
        SC7A20_CTRL6_H_LACTIVE, SC7A20_CTRL6_H_LACTIVE);
    SC7A20_ByteModify(SC7A20_CTRG_CTRL5,
        SC7A20_CTRL5_LIR_INT1 | SC7A20_CTRL5_LIR_INT2,
        SC7A20_AlgoConfig.motion_wake ? SC7A20_CTRL5_LIR_INT1 : 0U);

    /* LIS2DH12 forbids LPen=1 with HR=1. Normal RUN mode uses HR=1,
     * so clear it BEFORE enabling LPen for the 25Hz wake monitor.
     * The former illegal mode can inhibit 6D/HPF event recognition. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL4, SC7A20_CTRL4_HR, 0U);

    if (SC7A20_AlgoConfig.motion_wake) {
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_THS, SC7A20_MOTION_THS_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_DURATION, SC7A20_MOTION_DUR_REG);
        /* Keep INT1_CFG=0 through HPF startup; re-enable just before
         * connecting the output to PB8 in the sleep entry path. */
        /* CTRL2=0x01：只将高通送 AOI1，AOI2 的 6D 姿态判断保留重力。
         * HPM=00 配合布防前读 REFERENCE 清除静态偏置。 */
        SC7A20_ByteWrite(SC7A20_CTRG_CTRL2, SC7A20_CTRL2_HPIS1);
        routes |= SC7A20_CTRL3_I1_AOI1;
    }

    if (SC7A20_AlgoConfig.pickup_wake) {
        SC7A20_ByteWrite(SC7A20_CTRG_INT2_THS, SC7A20_PICKUP_THS_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT2_DURATION, SC7A20_PICKUP_DUR_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT2_CFG,
            SC7A20_INTMODE_6D_MOTION |
            SC7A20_INT_CFG_XHIE | SC7A20_INT_CFG_XLIE |
            SC7A20_INT_CFG_YHIE | SC7A20_INT_CFG_YLIE |
            SC7A20_INT_CFG_ZHIE | SC7A20_INT_CFG_ZLIE);
        routes |= SC7A20_CTRL3_I1_AOI2;
    }

    /* Motion 在 25Hz 下使用 768mg / 连续约80ms 的试验门限；
     * 维持 Pickup 已验证的 25Hz/6D 配置，不直接照搬文章的10Hz/500ms。 */
    SC7A20_ByteWrite(SC7A20_CTRG_CTRL1,
        SC7A20_DEFAULT_ODR | SC7A20_CTRL1_LPEN | SC7A20_DEFAULT_AXIS_EN);

    /* Wait long enough for either ODR's HPF and 6D engines to settle;
     * routing INT1 immediately previously caused false wake-up loops. */
    vTaskDelay(pdMS_TO_TICKS(420U));
    if (SC7A20_AlgoConfig.motion_wake)
        SC7A20_ByteRead(SC7A20_CTRG_REFERENCE, &scratch); /* HPF DC reset */
    vTaskDelay(pdMS_TO_TICKS(120U));
    SC7A20_ByteRead(SC7A20_STRG_INT1_SOURCE, &scratch);
    SC7A20_ByteRead(SC7A20_STRG_INT2_SOURCE, &scratch);
    /* Write a fresh AOI1 config only after filter settling and source reads.
     * Previously INT1_CFG was enabled before the 420+120ms delay. */
    if (SC7A20_AlgoConfig.motion_wake)
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_CFG, SC7A20_INTMODE_OR |
            SC7A20_INT_CFG_XHIE | SC7A20_INT_CFG_YHIE |
            SC7A20_INT_CFG_ZHIE);
    SC7A20_ByteModify(SC7A20_CTRG_CTRL3,
        SC7A20_CTRL3_I1_AOI1 | SC7A20_CTRL3_I1_AOI2, routes);

    /* Pickup 为短脉冲，Motion 已锁存：连续低电平可能是一次合法的
     * Motion 事件，也可能是持续判定/共享 IRQ 故障。达到保护门限后
     * 先读 SOURCE 释放锁存，再复查 PB8，仍低才关闭传感器唤醒。 */
    {
        uint8_t low_streak = 0U;
        for (uint8_t i = 0U; i < 30U; ++i) {
            vTaskDelay(pdMS_TO_TICKS(13U));
            if (gpio_input_data_bit_read(GPIOB, GPIO_PINS_8) == RESET) {
                if (++low_streak >= 20U) break;
            } else {
                low_streak = 0U;
            }
        }
        if (low_streak >= 20U) {
            /* 先释放可能的锁存；仍低则关闭传感器唤醒，防止卡住 PB8。 */
            SC7A20_ByteRead(SC7A20_STRG_INT1_SOURCE, &scratch);
            SC7A20_ByteRead(SC7A20_STRG_INT2_SOURCE, &scratch);
            vTaskDelay(pdMS_TO_TICKS(13U));
            if (gpio_input_data_bit_read(GPIOB, GPIO_PINS_8) == RESET) {
                sc7a20_wake_routes_off();
                SC7A20_LowPowerSet();
            }
        }
    }
}

void SC7A20_AlgoPmResume(void *ctx)
{
    (void)ctx;
    if (SC7A20_IsInitialized()) {
        /* 先读中断源解除 AOI1 锁存，之后撤销路由；此处不能读 REFERENCE。 */
        volatile uint8_t src = 0U;
        SC7A20_ByteRead(SC7A20_STRG_INT1_SOURCE, &src);
        SC7A20_ByteRead(SC7A20_STRG_INT2_SOURCE, &src);
        /* Clears INT1_CFG after wake, having captured its event source. */
        sc7a20_wake_routes_off(); /* Release shared PB8 before returning. */
        /* Power-down mode removed XYZ enable bits; restore all of them. */
        SC7A20_ByteWrite(SC7A20_CTRG_CTRL1,
            SC7A20_DEFAULT_ODR | SC7A20_DEFAULT_AXIS_EN);
        /* Once LPen is cleared, return to the 12-bit HR=1 RUN mode.
         * Respect the configurable default if HR is disabled in future. */
        SC7A20_ByteModify(SC7A20_CTRG_CTRL4, SC7A20_CTRL4_HR,
                           SC7A20_DEFAULT_HIGHRES ? SC7A20_CTRL4_HR : 0U);
        /* TEMP_CFG=0xC0 follows LIS2DH-like ADC_EN/TEMP_EN semantics.
         * Silan v0.4 register list has no bit description: TODO_HW_VERIFY. */
        SC7A20_ByteWrite(SC7A20_CTRG_TEMP_CFG, 0xC0U);
    } else {
        SC7A20_Init();
    }
}
/*********************** wake monitor end *******************************/
