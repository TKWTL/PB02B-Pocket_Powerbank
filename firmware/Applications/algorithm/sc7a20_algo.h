/*
 * sc7a20_algo.h - SC7A20 application policy/state machines
 *
 * load_task feeds orientation samples; UI only changes RAM policy flags.
 * PM suspend/resume hooks are the only algorithm entry points that may perform
 * SC7A20 I2C writes, and are called by pm_device while the bus is available.
 */
#ifndef SC7A20_ALGO_H
#define SC7A20_ALGO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SC7A20_ORIENT_UNKNOWN = 0,
    SC7A20_ORIENT_NORMAL,     /* X ≈ -1g：当前显示方向 */
    SC7A20_ORIENT_FLIPPED     /* X ≈ +1g：翻转180° */
} sc7a20_orientation_t;

typedef struct {
    uint8_t pickup_wake;   /* 10Hz硬件AOI2/6D Movement -> INT1，RAM开关 */
    uint8_t motion_wake;   /* 10Hz硬件AOI1+HPF -> INT1，RAM开关 */
} sc7a20_algo_config_t;

extern sc7a20_algo_config_t SC7A20_AlgoConfig;

/* Wake monitor first-board tuning at FS=2g (16mg/LSB for INT1/INT2_THS).
 * 6D Movement detects orientation transitions, not pure translation
 * while retaining an unchanged orientation; validate each mode on board. */
#define SC7A20_PICKUP_THS_REG        32U   /* 512mg, 6D direction crossing */
#define SC7A20_MOTION_THS_REG        16U   /* 256mg, HP-filtered motion */
#define SC7A20_WAKE_DUR_REG           1U   /* one ODR period: 100ms at 10Hz */
#define SC7A20_ALGO_SAMPLE_MS         200U   /* 5Hz application sampling */
#define SC7A20_ALGO_ENTER_MG        650.0f
#define SC7A20_ALGO_CONFIRM_SAMPLES   10U    /* 10 further 200ms samples = 2s stable */

void SC7A20_AlgoInit(void);
void SC7A20_AlgoUpdate(float x_mg);
void SC7A20_AlgoLoadSample(void); /* 5Hz调用：AccelLoad + 姿态算法 */
sc7a20_orientation_t SC7A20_AlgoGetOrientation(void);

/* pm_device callbacks: framework只负责调度，SC7A20具体低功耗策略归算法层。 */
void SC7A20_AlgoPmSuspend(void *ctx);
void SC7A20_AlgoPmResume(void *ctx);
extern volatile uint32_t sc7a20_wake_arm_fail_count;

#ifdef __cplusplus
}
#endif

#endif /* SC7A20_ALGO_H */
