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
/* Debugger Watch: sleep attempts rejected because IRQ remained low. */
volatile uint32_t sc7a20_wake_arm_fail_count;
volatile uint32_t sc7a20_wake_arm_ok_count;
volatile uint8_t sc7a20_wake_last_src1, sc7a20_wake_last_src2;
volatile uint8_t sc7a20_wake_arm_ctrl2, sc7a20_wake_arm_ctrl3;
volatile uint8_t sc7a20_wake_arm_ctrl1, sc7a20_wake_arm_int1_cfg;
volatile uint8_t sc7a20_wake_arm_pb8; /* physical shared IRQ level: 1=idle */
volatile uint32_t sc7a20_wake_resume_count;

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
 * Shared PB8/EXTI8 is low-active; ISR always wakes without classifying source.
 * Motion: AOI1 (0x30) XYZ OR + high-pass (CTRL2.HPIS1), INT1 CTRL3[6].
 * Pickup: AOI2 (0x34) 6D Movement XYZ axes, INT1 CTRL3[5].
 * Both engines share 25Hz / 2g LP ODR. Motion HPM=11 automatically
 * resets HPF on interrupt (LIS2DH12-compatible experimental baseline).
 * Reading REFERENCE on resume in HPM=00 regressed to zero wake events;
 * restore prior HPM behavior before isolating the recurring-wake fault.
 * PB8 interrupt remains non-latched to coexist with SW6306.
 */
static void sc7a20_wake_routes_off(void)
{
    /* Mask IRQ routes before reprogramming threshold/modes. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL3,
        SC7A20_CTRL3_I1_AOI1 | SC7A20_CTRL3_I1_AOI2, 0U);
    SC7A20_ByteWrite(SC7A20_CTRG_INT1_CFG, 0U);
    SC7A20_ByteWrite(SC7A20_CTRG_INT2_CFG, 0U);
    /* Also restore normal-reset HPF mode before the next sleep session. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL2,
        SC7A20_CTRL2_HPM_MSK | SC7A20_CTRL2_HPIS1, 0U);
}

void SC7A20_AlgoPmSuspend(void *ctx)
{
    uint8_t routes = 0U;
    volatile uint8_t scratch;
    (void)ctx;

    if (!SC7A20_IsInitialized() && !pm_api_transport_mode_requested() &&
        (SC7A20_AlgoConfig.pickup_wake || SC7A20_AlgoConfig.motion_wake))
        SC7A20_Init();

    if (!SC7A20_IsInitialized()) return;
    sc7a20_wake_routes_off();
    /* No temperature ADC needed while the CPU is asleep.
     * TEMP_CFG bit semantics follow LIS2DH compatibility, unverified. */
    SC7A20_ByteWrite(SC7A20_CTRG_TEMP_CFG, 0U);

    if (pm_api_transport_mode_requested() ||
        (!SC7A20_AlgoConfig.pickup_wake && !SC7A20_AlgoConfig.motion_wake)) {
        SC7A20_LowPowerSet(); /* ODR=0, existing minimum-power path */
        return;
    }

    /* Keep low-active output (idle high) on shared PB8. Do not latch AOI. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL6,
        SC7A20_CTRL6_H_LACTIVE, SC7A20_CTRL6_H_LACTIVE);
    /* Ensure both AOI generators are non-latched, including AOI2 routed to INT1. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL5,
        SC7A20_CTRL5_LIR_INT1 | SC7A20_CTRL5_LIR_INT2, 0U);

    /* LIS2DH12 forbids LPen=1 with HR=1. Normal RUN mode uses HR=1,
     * so clear it BEFORE enabling LPen for the 10/25Hz wake monitor.
     * The former illegal mode can inhibit 6D/HPF event recognition. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL4, SC7A20_CTRL4_HR, 0U);

    if (SC7A20_AlgoConfig.motion_wake) {
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_THS, SC7A20_MOTION_THS_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_DURATION, SC7A20_MOTION_DUR_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_CFG, SC7A20_INTMODE_OR |
            SC7A20_INT_CFG_XHIE | SC7A20_INT_CFG_XLIE |
            SC7A20_INT_CFG_YHIE | SC7A20_INT_CFG_YLIE |
            SC7A20_INT_CFG_ZHIE | SC7A20_INT_CFG_ZLIE);
        /* Restore HPM=11 autoreset on interrupt. Prior HPM=00 plus a
         * resume REFERENCE read prevented even the first Motion wake. */
        SC7A20_ByteModify(SC7A20_CTRG_CTRL2,
            SC7A20_CTRL2_HPM_MSK | SC7A20_CTRL2_HPIS1,
            SC7A20_HPM_AUTORST | SC7A20_CTRL2_HPIS1);
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

    /* Motion: 25Hz, 6LSB (96mg), duration 0, HPF autoreset.
     * Pickup threshold and 6D mode are unchanged. */
    SC7A20_ByteWrite(SC7A20_CTRG_CTRL1,
        SC7A20_ODR_25HZ | SC7A20_CTRL1_LPEN | SC7A20_DEFAULT_AXIS_EN);

    /* Wait long enough for either ODR's HPF and 6D engines to settle;
     * routing INT1 immediately previously caused false wake-up loops. */
    vTaskDelay(pdMS_TO_TICKS(420U));
    if (SC7A20_AlgoConfig.motion_wake)
        SC7A20_ByteRead(SC7A20_CTRG_REFERENCE, &scratch);
    vTaskDelay(pdMS_TO_TICKS(120U));
    SC7A20_ByteRead(SC7A20_STRG_INT1_SOURCE, &scratch);
    SC7A20_ByteRead(SC7A20_STRG_INT2_SOURCE, &scratch);
    SC7A20_ByteModify(SC7A20_CTRG_CTRL3,
        SC7A20_CTRL3_I1_AOI1 | SC7A20_CTRL3_I1_AOI2, routes);

    /* Preserve brief wake pulses; disarm only if INT1 remains asserted
     * for three consecutive checks. */
    {
        uint8_t low_streak = 0U;
        for (uint8_t i = 0U; i < 3U; ++i) {
            vTaskDelay(pdMS_TO_TICKS(120U));
            if (gpio_input_data_bit_read(GPIOB, GPIO_PINS_8) == RESET)
                ++low_streak;
            else
                low_streak = 0U;
        }
        if (low_streak == 3U) {
            ++sc7a20_wake_arm_fail_count;
            sc7a20_wake_routes_off();
            SC7A20_LowPowerSet();
        } else {
            ++sc7a20_wake_arm_ok_count;
        }
    }
    /* Verify actual silicon state, not merely software policy. Sample
     * AFTER the PB8 fail-closed guard to catch a disarmed second sleep. */
    SC7A20_ByteRead(SC7A20_CTRG_CTRL1, &sc7a20_wake_arm_ctrl1);
    SC7A20_ByteRead(SC7A20_CTRG_CTRL2, &sc7a20_wake_arm_ctrl2);
    SC7A20_ByteRead(SC7A20_CTRG_CTRL3, &sc7a20_wake_arm_ctrl3);
    SC7A20_ByteRead(SC7A20_CTRG_INT1_CFG, &sc7a20_wake_arm_int1_cfg);
    sc7a20_wake_arm_pb8 = gpio_input_data_bit_read(GPIOB, GPIO_PINS_8) == SET;
}

void SC7A20_AlgoPmResume(void *ctx)
{
    (void)ctx;
    if (SC7A20_IsInitialized()) {
        /* Capture event sources before disabling routes; remains readable
         * in Watch after the MCU wakes by either sensor or physical button. */
        volatile uint8_t src = 0U;
        SC7A20_ByteRead(SC7A20_STRG_INT1_SOURCE, &src);
        sc7a20_wake_last_src1 = src;
        SC7A20_ByteRead(SC7A20_STRG_INT2_SOURCE, &src);
        sc7a20_wake_last_src2 = src;
        /* HPM=11 resets the HPF on interrupt; do not read REFERENCE
         * during resume (the last trial regressed to no Motion wake). */
        ++sc7a20_wake_resume_count;
        sc7a20_wake_routes_off(); /* Release shared PB8 before returning. */
        /* Power-down mode removed XYZ enable bits; restore all of them. */
        SC7A20_ByteWrite(SC7A20_CTRG_CTRL1,
            SC7A20_ODR_10HZ | SC7A20_DEFAULT_AXIS_EN);
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
