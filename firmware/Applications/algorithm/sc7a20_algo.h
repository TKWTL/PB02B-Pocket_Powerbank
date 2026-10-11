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
    uint8_t pickup_wake;   /* AOI2/6D -> INT1；单独启用为10Hz */
    uint8_t motion_wake;   /* AOI1+HPF -> INT1；实验阶段25Hz（共用ODR） */
} sc7a20_algo_config_t;

extern sc7a20_algo_config_t SC7A20_AlgoConfig;

/* 10/25Hz, FS=2g -> INTx_THS LSB=16mg (LIS2DH12-compatible; verify on SC7A20).
 * 6D: return to 32LSB=512mg: the previous 54LSB value lost all observed
 * pickup events. Revisit after validating actual INT2_SOURCE responses.
 * Motion: 12LSB=192mg, zero duration catches single 10Hz samples.
 * In low-power mode CTRL4.HR MUST be 0 (LPen=1/HR=1 is invalid). */
#define SC7A20_PICKUP_THS_REG        32U
#define SC7A20_PICKUP_DUR_REG         1U
#define SC7A20_MOTION_THS_REG        12U
#define SC7A20_MOTION_DUR_REG         0U
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
extern volatile uint32_t sc7a20_wake_arm_ok_count;
extern volatile uint8_t sc7a20_wake_last_src1, sc7a20_wake_last_src2;

#ifdef __cplusplus
}
#endif

#endif /* SC7A20_ALGO_H */
