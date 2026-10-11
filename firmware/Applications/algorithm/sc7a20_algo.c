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
 * Both use 10Hz / 2g low-power mode (LPen=1, HR=0), non-latched IRQ to avoid
 * holding the shared SW6306 line low. Thresholds need board confirmation.
 * No added CPU classification or sampling-rate escalation on wake.
 */
static void sc7a20_wake_routes_off(void)
{
    /* Mask IRQ routes before reprogramming threshold/modes. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL3,
        SC7A20_CTRL3_I1_AOI1 | SC7A20_CTRL3_I1_AOI2, 0U);
    SC7A20_ByteWrite(SC7A20_CTRG_INT1_CFG, 0U);
    SC7A20_ByteWrite(SC7A20_CTRG_INT2_CFG, 0U);
    SC7A20_ByteModify(SC7A20_CTRG_CTRL2, SC7A20_CTRL2_HPIS1, 0U);
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
    SC7A20_ByteModify(SC7A20_CTRG_CTRL5,
        SC7A20_CTRL5_LIR_INT1, 0U);

    /* LIS2DH12 forbids LPen=1 with HR=1. Normal RUN mode uses HR=1,
     * so clear it BEFORE enabling LPen for the 10Hz wake monitor.
     * The former illegal mode can inhibit 6D/HPF event recognition. */
    SC7A20_ByteModify(SC7A20_CTRG_CTRL4, SC7A20_CTRL4_HR, 0U);

    if (SC7A20_AlgoConfig.motion_wake) {
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_THS, SC7A20_MOTION_THS_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_DURATION, SC7A20_MOTION_DUR_REG);
        SC7A20_ByteWrite(SC7A20_CTRG_INT1_CFG, SC7A20_INTMODE_OR |
            SC7A20_INT_CFG_XHIE | SC7A20_INT_CFG_XLIE |
            SC7A20_INT_CFG_YHIE | SC7A20_INT_CFG_YLIE |
            SC7A20_INT_CFG_ZHIE | SC7A20_INT_CFG_ZLIE);
        SC7A20_ByteModify(SC7A20_CTRG_CTRL2,
            SC7A20_CTRL2_HPIS1, SC7A20_CTRL2_HPIS1);
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

    /* 10Hz low-power engine, XYZ all enabled. Reset high-pass reference
     * and clear any stale sources BEFORE connecting generators to INT1. */
    SC7A20_ByteWrite(SC7A20_CTRG_CTRL1,
        SC7A20_ODR_10HZ | SC7A20_CTRL1_LPEN | SC7A20_DEFAULT_AXIS_EN);

    /* At 10Hz the high-pass and 6D engines need several samples after
     * startup. Routing INT1 immediately produced false wake-up loops. */
    vTaskDelay(pdMS_TO_TICKS(420U));
    if (SC7A20_AlgoConfig.motion_wake)
        SC7A20_ByteRead(SC7A20_CTRG_REFERENCE, &scratch);
    vTaskDelay(pdMS_TO_TICKS(120U));
    SC7A20_ByteRead(SC7A20_STRG_INT1_SOURCE, &scratch);
    SC7A20_ByteRead(SC7A20_STRG_INT2_SOURCE, &scratch);
    SC7A20_ByteModify(SC7A20_CTRG_CTRL3,
        SC7A20_CTRL3_I1_AOI1 | SC7A20_CTRL3_I1_AOI2, routes);

    /* Shared PB8 must be released before WFI. Fail closed for this sleep
     * if INT1 stays low. This does NOT fix push-pull wiring contention. */
    for (uint8_t i = 0U; i < 3U; ++i) {
        vTaskDelay(pdMS_TO_TICKS(120U));
        if (gpio_input_data_bit_read(GPIOB, GPIO_PINS_8) == RESET) {
            ++sc7a20_wake_arm_fail_count;
            sc7a20_wake_routes_off();
            SC7A20_LowPowerSet();
            break;
        }
    }
}

void SC7A20_AlgoPmResume(void *ctx)
{
    (void)ctx;
    if (SC7A20_IsInitialized()) {
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
