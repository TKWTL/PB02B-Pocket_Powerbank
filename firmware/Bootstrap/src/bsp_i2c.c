#include "bsp_i2c.h"
#include "bsp_usart.h"   /* 事务失败打点：USART_SendByte('!') */

/* Workbench can omit wk_i2c_app_init() when I2C Application is unchecked.
 * Bind the I2C1 handle at startup, independently of generated main.c. */
i2c_handle_type hi2c1 = { .i2cx = I2C1 };

void wk_i2c_app_init(void)
{
    hi2c1.i2cx = I2C1;
}

TaskHandle_t i2c_wait_task = NULL;   // 当前等待I2C完成的任务句柄

/* I2C 故障现场（sticky）：在错误中断入口抓取，早于官方 i2c_err_irq_handler()
 * 清标志，因此调试器 Watch 中能看到“最初的错误现场”。复位后自动清零。 */
volatile uint32_t i2c_fault_sts;
volatile uint32_t i2c_fault_ctrl1;
volatile uint32_t i2c_fault_ctrl2;
volatile uint32_t i2c_fault_txdt;
volatile uint32_t i2c_fault_count;
volatile uint32_t i2c_fault_code;
volatile uint32_t i2c_recover_count;
volatile uint32_t i2c_recover_reason;

void i2c_fault_capture(void)
{
    i2c_fault_sts   = I2C1->sts;
    i2c_fault_ctrl1 = I2C1->ctrl1;
    i2c_fault_ctrl2 = I2C1->ctrl2;
    i2c_fault_txdt  = I2C1->txdt;
    i2c_fault_count++;
}

/* ISR 风暴判定（纯计数器，ISR 内不打印）：调试器 Watch 用。
 * 判据：一次"卡死"后若 evt_cnt 疯涨而 FreeRTOS 的 tick 基本不动，即为中断风暴。 */
volatile uint32_t i2c_evt_irq_cnt;
volatile uint32_t i2c_err_irq_cnt;
/* 最近一次 ISR 快照：卡死后看这里能判断当时的 status/pcount/mode/error */
volatile uint32_t i2c_last_sts;
volatile uint16_t i2c_last_pcount;
volatile uint16_t i2c_last_psize;
volatile uint8_t  i2c_last_mode;
volatile uint32_t i2c_last_error;

/* I2C 总线故障恢复：复位 I2C1 外设并重新初始化。
 * 背景：从机拉死总线 / 上次传输异常未复位时 BUSYF 持续为 1，后续所有传输
 * 在启动阶段（i2c_wait_flag 轮询 BUSYF/TDIS/TDC）即超时返回错误，此时中断
 * 从未使能、不会有完成通知；若调用方永久等通知会持锁卡死整机（曾实测：
 * 全任务阻塞只剩空闲任务）。复位清 BUSYF 与错误标志，总线自动恢复；
 * NVIC 中断使能不受外设复位影响，无需重新使能。 */
static void i2c_bus_recover(uint32_t reason, uint32_t detail)
{
    i2c_recover_count++;
    i2c_recover_reason = (reason << 16) | (detail & 0xFFFFU);
    /* 先关 I2C 事件/错误中断，避免复位过程中 ISR 访问半复位状态 */
    i2c_interrupt_enable(I2C1, I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | I2C_ACKFIAL_INT | I2C_TD_INT | I2C_RD_INT, FALSE);
    i2c_enable(I2C1, FALSE);
    i2c_reset(I2C1);                      /* 外设复位：清 BUSYF/错误标志 */
    NVIC_ClearPendingIRQ(I2C1_EVT_IRQn);
    NVIC_ClearPendingIRQ(I2C1_ERR_IRQn);
    if (hi2c1.error_code != I2C_OK) {
        i2c_fault_code = (uint32_t)hi2c1.error_code;   /* 保留错误码（TDC/TCRLD/INTERRUPT/ACKFAIL…）供 Watch 判读 */
    }
    hi2c1.status = 0;                 /* 清 handle 残留传输状态（0=I2C_START） */
    hi2c1.error_code = I2C_OK;
    wk_i2c1_init();                       /* 重新初始化 GPIO/时钟/寄存器 */
}

/* ==================== 事务监督器 ====================
 * 语义约定（关键）：中断通知只表示"本次等待条件已结束"——正常完成与错误中断都会唤醒，
 * 所以【收到通知 ≠ 成功】。成功必须由 status==I2C_END 且 error_code==I2C_OK 双确认
 * （该库在 ACKFAIL/TDC/TCRLD 等错误后也会把 status 置为 I2C_END）。
 * 失败处理：整笔重试，而不是从断点续传——恢复总线后重新调用库的 *_int() 接口，
 * 由它重新装填 addr/reg/pbuff/pcount/error_code，从第 0 字节重新开始。
 * 每次整笔失败输出一个 '!'，便于串口定位（成功路径零输出）。 */
#define BSP_I2C_ATTEMPTS   3U   /* 1 次初始 + 2 次重试（失败后重试 2 次） */

#define BSP_I2C_OP_MEM_RD  0U
#define BSP_I2C_OP_MEM_WR  1U
#define BSP_I2C_OP_RD      2U
#define BSP_I2C_OP_WR      3U

static i2c_status_type i2c_exec(uint8_t kind, uint8_t addr, uint8_t reg,
                                uint8_t *p_buf, uint16_t len)
{
    i2c_status_type last = I2C_OK;   /* 最近一次失败码：必须在 recover 之前捕获——
                                      * i2c_bus_recover() 会把 error_code 清成 I2C_OK，
                                      * 若在末尾再读它就会把"失败"当成功返回。 */
    uint8_t attempt;
    for (attempt = 0U; attempt < BSP_I2C_ATTEMPTS; attempt++) {
        i2c_status_type rc;

        i2c_wait_task = xTaskGetCurrentTaskHandle();
        (void)ulTaskNotifyTake(pdTRUE, 0);          /* 清上一笔残留通知 */

        switch (kind) {
        case BSP_I2C_OP_MEM_RD:
            rc = i2c_memory_read_int(&hi2c1, I2C_MEM_ADDR_WIDIH_8, addr, reg, p_buf, len, BSP_I2C_START_WAIT_CNT);
            break;
        case BSP_I2C_OP_MEM_WR:
            rc = i2c_memory_write_int(&hi2c1, I2C_MEM_ADDR_WIDIH_8, addr, reg, p_buf, len, BSP_I2C_START_WAIT_CNT);
            break;
        case BSP_I2C_OP_RD:
            rc = i2c_master_receive_int(&hi2c1, addr, p_buf, len, BSP_I2C_START_WAIT_CNT);
            break;
        default:
            rc = i2c_master_transmit_int(&hi2c1, addr, p_buf, len, BSP_I2C_START_WAIT_CNT);
            break;
        }

        if (rc != I2C_OK) {
            last = rc;
            USART_SendByte('!');
            i2c_bus_recover(BSP_I2C_RC_START_FAIL, (uint32_t)rc);
            continue;
        }
        /* 等通知超时：总线卡死（中断未使能则永不来通知） */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BSP_I2C_SESSION_TIMEOUT_MS)) == 0) {
            last = I2C_ERR_TIMEOUT;
            USART_SendByte('!');
            i2c_bus_recover(BSP_I2C_RC_NOTIFY_TIMEOUT, 0);
            continue;
        }
        /* 通知到达但事务状态异常（错误中断也会唤醒，必须查 error_code） */
        if (hi2c1.status == BSP_I2C_STATUS_END && hi2c1.error_code == I2C_OK) {
            i2c_wait_task = NULL;
            return I2C_OK;
        }
        last = (hi2c1.error_code != I2C_OK) ? hi2c1.error_code : I2C_ERR_INTERRUPT;
        USART_SendByte('!');
        i2c_bus_recover(BSP_I2C_RC_TRANS_ERROR, (uint32_t)hi2c1.error_code);
    }
    i2c_wait_task = NULL;
    return last;                  /* 两次都失败：返回最后一次库错误码（HAL 风格） */
}

/* 发送函数：整笔事务最多尝试 BSP_I2C_ATTEMPTS 次，失败返回非 I2C_OK */
i2c_status_type I2C_Transmit(uint8_t addr, uint8_t* p_buf, uint16_t len){
    return i2c_exec(BSP_I2C_OP_WR, addr, 0U, p_buf, len);
}

/* 带寄存器设置的发送函数 */
i2c_status_type I2C_RegWrite(uint8_t addr, uint8_t reg, uint8_t* p_buf, uint16_t len){
    return i2c_exec(BSP_I2C_OP_MEM_WR, addr, reg, p_buf, len);
}

/* 读取函数 */
i2c_status_type I2C_Receive(uint8_t addr, uint8_t* p_buf, uint16_t len){
    return i2c_exec(BSP_I2C_OP_RD, addr, 0U, p_buf, len);
}

/* 带寄存器设置的读取函数 */
i2c_status_type I2C_RegRead(uint8_t addr, uint8_t reg, uint8_t* p_buf, uint16_t len){
    return i2c_exec(BSP_I2C_OP_MEM_RD, addr, reg, p_buf, len);
}


//确认地址存在函数
uint8_t I2C_IsReady(uint8_t addr){
    uint8_t ready, i2cbuf;
    xSemaphoreTake(mutex_i2c_handle, portMAX_DELAY);
    if(i2c_master_transmit(&hi2c1, addr, &i2cbuf, 1, 0x249F0) == I2C_OK) ready = 1;
    else ready = 0;
    xSemaphoreGive(mutex_i2c_handle);
    return ready;
}



/**
  **************************************************************************
  * @file     i2c_application.c
  * @brief    the driver library of the i2c peripheral
  **************************************************************************
  *
  * Copyright (c) 2025, Artery Technology, All rights reserved.
  *
  * The software Board Support Package (BSP) that is made available to
  * download from Artery official website is the copyrighted work of Artery.
  * Artery authorizes customers to use, copy, and distribute the BSP
  * software and its related documentation for the purpose of design and
  * development in conjunction with Artery microcontrollers. Use of the
  * software is governed by this copyright notice and the following disclaimer.
  *
  * THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS WITHOUT WARRANTIES,
  * GUARANTEES OR REPRESENTATIONS OF ANY KIND. ARTERY EXPRESSLY DISCLAIMS,
  * TO THE FULLEST EXTENT PERMITTED BY LAW, ALL EXPRESS, IMPLIED OR
  * STATUTORY OR OTHER WARRANTIES, GUARANTEES OR REPRESENTATIONS,
  * INCLUDING BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
  *
  **************************************************************************
  */

/* ---------------------------------------------------------------------------
 * I2C1 master-only subset from Artery Technology i2c_application.c (2025).
 * Copyright (c) 2025 Artery Technology. Licensed for development with Artery
 * microcontrollers under the source BSP notice. Only blocking address probe
 * and interrupt-driven TX/RX + 8-bit register accesses are needed here.
 *
 * Keep our tested ISR protections here, NOT in Workbench-regenerated vendor
 * middleware: abort on counter underflow, impossible TDC/TCRLD, and error
 * flags that could otherwise trigger endless interrupt tail-chaining.
 * --------------------------------------------------------------------------- */
#define I2C_START 0U
#define I2C_END   1U
#define I2C_EVENT_CHECK_NONE     0U
#define I2C_EVENT_CHECK_ACKFAIL  1U
#define I2C_EVENT_CHECK_STOP     2U
#define I2C_TXN_INTERRUPT_MASK (I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | \
                                I2C_ACKFIAL_INT | I2C_TD_INT | I2C_RD_INT)

/* Retained: i2c_reset_ctrl2_register */
static void i2c_reset_ctrl2_register(i2c_handle_type* hi2c)
{
  hi2c->i2cx->ctrl2_bit.saddr   = 0;
  hi2c->i2cx->ctrl2_bit.readh10 = 0;
  hi2c->i2cx->ctrl2_bit.cnt     = 0;
  hi2c->i2cx->ctrl2_bit.rlden   = 0;
  hi2c->i2cx->ctrl2_bit.dir     = 0;
}

/* Retained: i2c_wait_flag */
static i2c_status_type i2c_wait_flag(i2c_handle_type* hi2c, uint32_t flag, uint32_t event_check, uint32_t timeout)
{
  if(flag == I2C_BUSYF_FLAG)
  {
    while(i2c_flag_get(hi2c->i2cx, flag) != RESET)
    {
      /* check timeout */
      if((timeout--) == 0)
      {
        hi2c->error_code = I2C_ERR_TIMEOUT;

        return I2C_ERR_TIMEOUT;
      }
    }
  }
  else
  {
    while(i2c_flag_get(hi2c->i2cx, flag) == RESET)
    {
      /* check the ack fail flag */
      if(event_check & I2C_EVENT_CHECK_ACKFAIL)
      {
        if(i2c_flag_get(hi2c->i2cx, I2C_ACKFAIL_FLAG) != RESET)
        {
          /* clear ack fail flag */
          i2c_flag_clear(hi2c->i2cx, I2C_ACKFAIL_FLAG);

          hi2c->error_code = I2C_ERR_ACKFAIL;

          return I2C_ERR_ACKFAIL;
        }
      }

      /* check the stop flag */
      if(event_check & I2C_EVENT_CHECK_STOP)
      {
        if(i2c_flag_get(hi2c->i2cx, I2C_STOPF_FLAG) != RESET)
        {
          /* clear stop flag */
          i2c_flag_clear(hi2c->i2cx, I2C_STOPF_FLAG);

          i2c_reset_ctrl2_register(hi2c);

          hi2c->error_code = I2C_ERR_STOP;

          return I2C_ERR_STOP;
        }
      }

      /* check timeout */
      if((timeout--) == 0)
      {
        hi2c->error_code = I2C_ERR_TIMEOUT;

        return I2C_ERR_TIMEOUT;
      }
    }
  }

  return I2C_OK;
}

/* Retained: i2c_start_transfer */
static void i2c_start_transfer(i2c_handle_type* hi2c, uint16_t address, i2c_start_mode_type start)
{
  if (hi2c->pcount > MAX_TRANSFER_CNT)
  {
    hi2c->psize = MAX_TRANSFER_CNT;

    i2c_transmit_set(hi2c->i2cx, address, hi2c->psize, I2C_RELOAD_MODE, start);
  }
  else
  {
    hi2c->psize = hi2c->pcount;

    i2c_transmit_set(hi2c->i2cx, address, hi2c->psize, I2C_AUTO_STOP_MODE, start);
  }
}

/* Retained: i2c_refresh_txdt_register */
static void i2c_refresh_txdt_register(i2c_handle_type* hi2c)
{
  /* clear tdis flag */
  if (i2c_flag_get(hi2c->i2cx, I2C_TDIS_FLAG) != RESET)
  {
    hi2c->i2cx->txdt = 0x00;
  }

  /* refresh txdt register*/
  if (i2c_flag_get(hi2c->i2cx, I2C_TDBE_FLAG) == RESET)
  {
    hi2c->i2cx->sts_bit.tdbe = 1;
  }
}

/* Retained: i2c_memory_address_send */
static i2c_status_type i2c_memory_address_send(i2c_handle_type* hi2c, i2c_mem_address_width_type mem_address_width, uint16_t mem_address, int32_t timeout)
{
  i2c_status_type err_code;
  
  if(mem_address_width == I2C_MEM_ADDR_WIDIH_8)
  {
    /* send memory address */
    i2c_data_send(hi2c->i2cx, mem_address & 0xFF);
  }
  else
  {
    /* send memory address */
    i2c_data_send(hi2c->i2cx, (mem_address >> 8) & 0xFF);  
    
    /* wait for the tdis flag to be set */
    err_code = i2c_wait_flag(hi2c, I2C_TDIS_FLAG, I2C_EVENT_CHECK_ACKFAIL, timeout);
    
    if(err_code != I2C_OK)
    {
      return err_code;
    }
    
    /* send memory address */
    i2c_data_send(hi2c->i2cx, mem_address & 0xFF);  
  }
  
  return I2C_OK;
}

/* Retained: i2c_master_transmit */
i2c_status_type i2c_master_transmit(i2c_handle_type* hi2c, uint16_t address, uint8_t* pdata, uint16_t size, uint32_t timeout)
{
  /* initialization parameters */
  hi2c->pbuff = pdata;
  hi2c->pcount = size;

  hi2c->error_code = I2C_OK;

  /* wait for the busy flag to be reset */
  if (i2c_wait_flag(hi2c, I2C_BUSYF_FLAG, I2C_EVENT_CHECK_NONE, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_1;
  }

  /* start transfer */
  i2c_start_transfer(hi2c, address, I2C_GEN_START_WRITE);

  while (hi2c->pcount > 0)
  {
    /* wait for the tdis flag to be set */
    if(i2c_wait_flag(hi2c, I2C_TDIS_FLAG, I2C_EVENT_CHECK_ACKFAIL, timeout) != I2C_OK)
    {
      return I2C_ERR_STEP_2;
    }

    /* send data */
    i2c_data_send(hi2c->i2cx, *hi2c->pbuff++);
    hi2c->psize--;
    hi2c->pcount--;

    if ((hi2c->psize == 0) && (hi2c->pcount != 0))
    {
      /* wait for the tcrld flag to be set  */
      if (i2c_wait_flag(hi2c, I2C_TCRLD_FLAG, I2C_EVENT_CHECK_ACKFAIL, timeout) != I2C_OK)
      {
        return I2C_ERR_STEP_3;
      }

      /* continue transfer */
      i2c_start_transfer(hi2c, address, I2C_WITHOUT_START);
    }
  }

  /* wait for the stop flag to be set  */
  if(i2c_wait_flag(hi2c, I2C_STOPF_FLAG, I2C_EVENT_CHECK_ACKFAIL, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_4;
  }

  /* clear stop flag */
  i2c_flag_clear(hi2c->i2cx, I2C_STOPF_FLAG);

  /* reset ctrl2 register */
  i2c_reset_ctrl2_register(hi2c);

  return I2C_OK;
}

/* Retained: i2c_master_transmit_int */
i2c_status_type i2c_master_transmit_int(i2c_handle_type* hi2c, uint16_t address, uint8_t* pdata, uint16_t size, uint32_t timeout)
{
  /* initialization parameters */
  hi2c->mode   = I2C_INT_MA_TX;
  hi2c->status = I2C_START;

  hi2c->pbuff  = pdata;
  hi2c->pcount = size;

  hi2c->error_code = I2C_OK;

  /* wait for the busy flag to be reset */
  if (i2c_wait_flag(hi2c, I2C_BUSYF_FLAG, I2C_EVENT_CHECK_NONE, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_1;
  }

  /* start transfer */
  i2c_start_transfer(hi2c, address, I2C_GEN_START_WRITE);

  /* enable interrupt */
  i2c_interrupt_enable(hi2c->i2cx, I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | I2C_ACKFIAL_INT | I2C_TD_INT, TRUE);

  return I2C_OK;
}

/* Retained: i2c_master_receive_int */
i2c_status_type i2c_master_receive_int(i2c_handle_type* hi2c, uint16_t address, uint8_t* pdata, uint16_t size, uint32_t timeout)
{
  /* initialization parameters */
  hi2c->mode   = I2C_INT_MA_RX;
  hi2c->status = I2C_START;

  hi2c->pbuff  = pdata;
  hi2c->pcount = size;

  hi2c->error_code = I2C_OK;

  /* wait for the busy flag to be reset */
  if (i2c_wait_flag(hi2c, I2C_BUSYF_FLAG, I2C_EVENT_CHECK_NONE, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_1;
  }

  /* start transfer */
  i2c_start_transfer(hi2c, address, I2C_GEN_START_READ);

  /* enable interrupt */
  i2c_interrupt_enable(hi2c->i2cx, I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | I2C_ACKFIAL_INT | I2C_RD_INT, TRUE);

  return I2C_OK;
}

/* Retained: i2c_memory_write_int */
i2c_status_type i2c_memory_write_int(i2c_handle_type* hi2c, i2c_mem_address_width_type mem_address_width, uint16_t address, uint16_t mem_address, uint8_t* pdata, uint16_t size, uint32_t timeout)
{
  /* initialization parameters */
  hi2c->mode   = I2C_INT_MA_TX;
  hi2c->status = I2C_START;

  hi2c->pbuff  = pdata;
  hi2c->pcount = size + mem_address_width;

  hi2c->error_code = I2C_OK;

  /* wait for the busy flag to be reset */
  if (i2c_wait_flag(hi2c, I2C_BUSYF_FLAG, I2C_EVENT_CHECK_NONE, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_1;
  }

  /* start transfer */
  i2c_start_transfer(hi2c, address, I2C_GEN_START_WRITE);

  /* wait for the tdis flag to be set */
  if(i2c_wait_flag(hi2c, I2C_TDIS_FLAG, I2C_EVENT_CHECK_ACKFAIL, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_2;
  }

  /* send memory address */
  if(i2c_memory_address_send(hi2c, mem_address_width, mem_address, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_3;
  }
  
  hi2c->psize--;
  hi2c->pcount--;

  /* enable interrupt */
  i2c_interrupt_enable(hi2c->i2cx, I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | I2C_ACKFIAL_INT | I2C_TD_INT, TRUE);

  return I2C_OK;
}

/* Retained: i2c_memory_read_int */
i2c_status_type i2c_memory_read_int(i2c_handle_type* hi2c, i2c_mem_address_width_type mem_address_width, uint16_t address, uint16_t mem_address, uint8_t* pdata, uint16_t size, uint32_t timeout)
{
  /* initialization parameters */
  hi2c->mode   = I2C_INT_MA_RX;
  hi2c->status = I2C_START;

  hi2c->pbuff  = pdata;
  hi2c->pcount = size;

  hi2c->error_code = I2C_OK;

  /* wait for the busy flag to be reset */
  if(i2c_wait_flag(hi2c, I2C_BUSYF_FLAG, I2C_EVENT_CHECK_NONE, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_1;
  }

  /* start transfer */
  i2c_transmit_set(hi2c->i2cx, address, mem_address_width, I2C_SOFT_STOP_MODE, I2C_GEN_START_WRITE);

  /* wait for the tdis flag to be set */
  if(i2c_wait_flag(hi2c, I2C_TDIS_FLAG, I2C_EVENT_CHECK_ACKFAIL, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_2;
  }

  /* send memory address */
  if(i2c_memory_address_send(hi2c, mem_address_width, mem_address, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_3;
  }

  /* wait for the tdc flag to be set */
  if (i2c_wait_flag(hi2c, I2C_TDC_FLAG, I2C_EVENT_CHECK_NONE, timeout) != I2C_OK)
  {
    return I2C_ERR_STEP_4;
  }

  /* start transfer */
  i2c_start_transfer(hi2c, address, I2C_GEN_START_READ);

  /* enable i2c interrupt */
  i2c_interrupt_enable(hi2c->i2cx, I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | I2C_ACKFIAL_INT | I2C_RD_INT, TRUE);

  return I2C_OK;
}

/* Retained: i2c_abort_isr */
static void i2c_abort_isr(i2c_handle_type* hi2c, i2c_status_type err)
{
  hi2c->error_code = err;
  hi2c->status     = I2C_END;          /* 关键：否则任务侧只能等 session timeout */

  /* 关掉所有会继续触发的本事务中断 */
  i2c_interrupt_enable(hi2c->i2cx, I2C_TXN_INTERRUPT_MASK, FALSE);
  /* 断掉可能仍在请求的 DMA */
  i2c_dma_enable(hi2c->i2cx, I2C_DMA_REQUEST_TX, FALSE);
  i2c_dma_enable(hi2c->i2cx, I2C_DMA_REQUEST_RX, FALSE);
}

/* Retained: i2c_master_irq_handler_int */
static i2c_status_type i2c_master_irq_handler_int(i2c_handle_type* hi2c)
{
  if (i2c_flag_get(hi2c->i2cx, I2C_ACKFAIL_FLAG) != RESET)
  {
    /* clear ackfail flag */
    i2c_flag_clear(hi2c->i2cx, I2C_ACKFAIL_FLAG);

    /* refresh tx register */
    i2c_refresh_txdt_register(hi2c);

    if(hi2c->pcount != 0)
    {
      hi2c->error_code = I2C_ERR_ACKFAIL;
    }
  }
  else if (i2c_flag_get(hi2c->i2cx, I2C_TDIS_FLAG) != RESET)
  {
    /* [BUGFIX] 必须防 pcount/psize 下溢：总线噪声或错误状态机之后若多来一次 TD
     * 事件，pcount(0)-- 会回绕成 65535，且 pbuff 继续越界读写 → RAM 破坏 /
     * HardFault。不可能状态一律走 ISR 止血。 */
    if ((hi2c->pcount == 0) || (hi2c->psize == 0))
    {
      i2c_abort_isr(hi2c, I2C_ERR_INTERRUPT);
      return I2C_ERR_INTERRUPT;
    }

    /* send data */
    i2c_data_send(hi2c->i2cx, *hi2c->pbuff++);
    hi2c->pcount--;
    hi2c->psize--;
  }
  else if (i2c_flag_get(hi2c->i2cx, I2C_TCRLD_FLAG) != RESET)
  {
    if ((hi2c->psize == 0) && (hi2c->pcount != 0))
    {
      /* continue transfer */
      i2c_start_transfer(hi2c, i2c_transfer_addr_get(hi2c->i2cx), I2C_WITHOUT_START);
    }
    else
    {
      /* [BUGFIX] 原实现只 return I2C_ERR_TCRLD：异常标志未清、中断未关，
       * 会与 TDC 异常同源地反复进入本 ISR（中断风暴）→ 整机假死。
       * 关闭本事务整组中断并记录错误码，由上层复位总线恢复。 */
      i2c_abort_isr(hi2c, I2C_ERR_TCRLD);
      return I2C_ERR_TCRLD;
    }
  }
  else if (i2c_flag_get(hi2c->i2cx, I2C_RDBF_FLAG) != RESET)
  {
    /* [BUGFIX] 同 TDIS：防 pcount/psize 下溢导致 pbuff 越界写（RAM 破坏） */
    if ((hi2c->pcount == 0) || (hi2c->psize == 0))
    {
      i2c_abort_isr(hi2c, I2C_ERR_INTERRUPT);
      return I2C_ERR_INTERRUPT;
    }

    /* read data */
    (*hi2c->pbuff++) = i2c_data_receive(hi2c->i2cx);
    hi2c->pcount--;
    hi2c->psize--;
  }
  else if (i2c_flag_get(hi2c->i2cx, I2C_TDC_FLAG) != RESET)
  {
    if (hi2c->pcount == 0)
    {
      if (hi2c->i2cx->ctrl2_bit.astopen == 0)
      {
        /* generate stop condtion */
        i2c_stop_generate(hi2c->i2cx);
      }
    }
    else
    {
      /* [BUGFIX] 原实现只 return I2C_ERR_TDC：既不清 TDC 标志也不关 TDC 中断，
       * TDC 持续置位 → CPU 反复进入本 ISR（中断风暴）→ 任务全部饿死（整机假死，
       * 任务侧超时恢复机制也因无法运行而失效）。
       * 关闭本事务整组中断并记录错误码，由上层复位总线恢复。 */
      i2c_abort_isr(hi2c, I2C_ERR_TDC);
      return I2C_ERR_TDC;
    }
  }
  else if (i2c_flag_get(hi2c->i2cx, I2C_STOPF_FLAG) != RESET)
  {
    /* clear stop flag */
    i2c_flag_clear(hi2c->i2cx, I2C_STOPF_FLAG);

    /* reset ctrl2 register */
    i2c_reset_ctrl2_register(hi2c);

    if (i2c_flag_get(hi2c->i2cx, I2C_ACKFAIL_FLAG) != RESET)
    {
      /* clear ackfail flag */
      i2c_flag_clear(hi2c->i2cx, I2C_ACKFAIL_FLAG);
    }

    /* refresh tx dt register */
    i2c_refresh_txdt_register(hi2c);

    /* disable interrupts */
    i2c_interrupt_enable(hi2c->i2cx, I2C_ERR_INT | I2C_TDC_INT | I2C_STOP_INT | I2C_ACKFIAL_INT | I2C_TD_INT | I2C_RD_INT, FALSE);

    /* transfer complete */
    hi2c->status = I2C_END;
  }

  return I2C_OK;
}

/* Only I2C1 master interrupt mode is built; slave, DMA and SMBus support
 * were not used and are deliberately omitted to reduce flash usage. */
void i2c_evt_irq_handler(i2c_handle_type *hi2c)
{
    if (hi2c->mode == I2C_INT_MA_TX || hi2c->mode == I2C_INT_MA_RX)
        (void)i2c_master_irq_handler_int(hi2c);
    else
        i2c_abort_isr(hi2c, I2C_ERR_INTERRUPT);
}

void i2c_err_irq_handler(i2c_handle_type *hi2c)
{
    /* Explicit constant-flag clears avoid a local lookup array in ISR. */
    if (i2c_flag_get(hi2c->i2cx, I2C_BUSERR_FLAG) != RESET)
        i2c_flag_clear(hi2c->i2cx, I2C_BUSERR_FLAG);
    if (i2c_flag_get(hi2c->i2cx, I2C_ARLOST_FLAG) != RESET)
        i2c_flag_clear(hi2c->i2cx, I2C_ARLOST_FLAG);
    if (i2c_flag_get(hi2c->i2cx, I2C_OUF_FLAG) != RESET)
        i2c_flag_clear(hi2c->i2cx, I2C_OUF_FLAG);
    if (i2c_flag_get(hi2c->i2cx, I2C_PECERR_FLAG) != RESET)
        i2c_flag_clear(hi2c->i2cx, I2C_PECERR_FLAG);
    if (i2c_flag_get(hi2c->i2cx, I2C_TMOUT_FLAG) != RESET)
        i2c_flag_clear(hi2c->i2cx, I2C_TMOUT_FLAG);
    if (i2c_flag_get(hi2c->i2cx, I2C_ALERTF_FLAG) != RESET)
        i2c_flag_clear(hi2c->i2cx, I2C_ALERTF_FLAG);
    /* Bugfix: official handler only disabled ERR_INT, leaving event IRQs
     * active and causing an ISR storm before the task can recover I2C1. */
    i2c_abort_isr(hi2c, I2C_ERR_INTERRUPT);
}
