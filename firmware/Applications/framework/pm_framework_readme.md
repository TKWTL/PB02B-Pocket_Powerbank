# PM Framework · 使用与移植

## 分层

- `pm_api.h`：应用/ISR入口。空闲计时刷新、休眠禁止位、手动睡眠、运输模式和总线门闩。
- `pm_sleep_timer.c`：空闲超时；允许暂停、恢复及显式刷新。
- `pm_policy.c`：根据 UI 活动、USB 状态、低电量、唤醒和 Standby 请求计算下一状态。
- `pm_controller.c`：状态迁移，先关闭普通任务的I²C读写门，等待总线互斥锁排空后调用设备钩子。
- `pm_device.h/.c`：无堆分配的设备注册表（最多16个），按 `order` 升序 prepare/suspend、降序 resume。
- `pm_device_builtin.c`：内置显示、SC7A20、SW6306、SD3078 设备与 AT32F423 MCU 的实际低功耗入口。
- `powerdown_task.c`：RTC计时、NVM落盘与控制器运行；不应出现 MCU 专有寄存器操作。

## 典型流程

1. RUN空闲超时 → UI_OFF：关闭显示等视觉反馈；等待下一阶段策略条件。
2. SLEEP_PREPARE：普通任务停止新总线操作，等待I²C/SPI互斥锁空闲；失败则取消休眠。
3. `pm_device_suspend_all()` 挂起外围器件；SD3078记录RTC睡眠起点。
4. `pm_device_enter_deepsleep()` 进入 AT32F423 DeepSleep（低功耗稳压器+WFI，保留SRAM/GPIO）。
5. 唤醒后 MCU device **先**恢复LDO/PLL/外设时钟/SysTick/FreeRTOS，再恢复RTC时间计量和设备 `resume_all()`，最后打开常规总线访问。
6. 运输模式为非返回型 Standby：丢失RAM，PA0/WKUP1唤醒后按重启路径初始化。

`PM_AT32_EXTRA_LOW_POWER` 当前默认为 **1**。需在实机上验证供电容差、唤醒响应、低功耗电流，并禁用与额外低功耗模式不兼容的 DeepSleep Debug。调试失败时可临时在编译定义中改为0回退。

## 接入一个设备

在 `pm_device_builtin.c` 或 `pm_device_register_linker_set()` 中提供静态 `pm_device_node_t`（字段次序见 `pm_device.h`）：

- `on_prepare`：仅做非破坏性的 UI 关闭/准备。
- `on_suspend`：配置器件自身的睡眠寄存器、中断唤醒源；确保总线被门控之后才进行此操作。
- `on_resume`：恢复器件采样率、外设工作模式、中断；不允许早于 MCU 时钟恢复。
- `on_deepsleep` / `on_standby`：通常只由平台 MCU device 实现，业务器件填0。
- `order`：按外设依赖决定，MCU 内置节点排在外设之后。

通过 `pm_device_register()` 注册并检查返回值（超过 `PM_DEVICE_MAX` 会失败）。注意 PM 回调不得在已关闭总线或 FreeRTOS 调度的阶段调用需阻塞的通用任务接口。

## 应用接口

- 活动刷新：`pm_api_refresh_idle()`。
- 显式深睡/运输：`pm_api_force_sleep()`、`pm_api_enter_transport_mode()`。
- 防休眠位：`pm_api_set_sleep_block(mask, 1)`；完成后用 `0` 配对解除，避免永久不能睡眠。
- 睡眠计时：`pm_api_set_sleep_timeout(sec)`；`sec=0` 表示不自动休眠。
- 唤醒数据：`pm_api_mark_wake_data_stale()` / `pm_api_request_data_refresh()` / `pm_api_data_refresh_done()`，避免唤醒后显示陈旧数据。
- `pm_api_sleep_gate_get()` 为总线流量禁用信号。采样/传感器任务启动新事务前应检查它。

## 更换 MCU

保留 `pm_policy`、`pm_controller`、`pm_device` 和 `pm_sleep_timer`；把 `pm_device_builtin.c` 中 AT32 专属的 HICK/PLL/LDO、SysTick、EXINT、GPIO/WKUP 和 WFI/Standby 替换为目标 MCU 相应逻辑。始终确认：(1) RTC 在深睡仍计时；(2) 至少一个可靠的按键/外设唤醒源；(3) 唤醒时先恢复系统时钟再允许调度及 I²C；(4) 运输模式和保持 RAM 的深睡不是同一种唤醒语义。

## 测试清单

测量RUN/UI_OFF/DeepSleep/Standby四态电流，验证按键/USB/SC7A20抬起唤醒、I²C总线抢占、长时间RTC补时、反复进出睡眠和充放电中禁止深睡。不要把低功耗模式选项的改动当作已经过实机验证。
