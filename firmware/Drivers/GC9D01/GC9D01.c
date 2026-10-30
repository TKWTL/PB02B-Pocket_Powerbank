/*
  ******************************************************************************
  * @file    	GC9D01.c
  * @brief   	GC9D01 单色 TFT 液晶驱动（本项目用 0.99" 160 x 40，RGB565）实现
  * @note    	LVGL 通过 Applications/LVGL/porting/lv_port_disp.c 调用本驱动，
  *          	只用到 GC9D01_Init / GC9D01_SetWindow / GC9D01_RamWrite；
  *          	背光 GC9D01_SetBL 与旋转 GC9D01_rotation 由显示移植层按需调用。
  *          	上游的字符/点阵绘制接口已删除（字体与点阵图不再随工程编译）。
  * @par 参考来源
  *          	上游：Golinskiy Konstantin 的 STM32 GC9D01 驱动库
  *          	作者：Golinskiy Konstantin  邮箱：golinskiy.konstantin\@gmail.com
  *          	初始化命令序列取自 GC9D01 数据手册（0.99" 160 x 40 模组）
  *          	（本文件为上游版本的中文化注释 + 裁剪版）
  ******************************************************************************
*/

#include <GC9D01.h>
#include "wk_system.h"   /* wk_delay_ms：LCD 供电上电后的 5ms 稳定等待 */


uint16_t GC9D01_X_Start = GC9D01_XSTART;	
uint16_t GC9D01_Y_Start = GC9D01_YSTART;

uint16_t GC9D01_Width = 0;
uint16_t GC9D01_Height = 0;



static void GC9D01_SendCmd(uint8_t Cmd);
static void GC9D01_SendData(uint8_t Data );
static void GC9D01_SendDataMASS(uint8_t* buff, size_t buff_size);
void GC9D01_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void GC9D01_RamWrite(uint8_t *pBuff, uint32_t Len);
static void GC9D01_ColumnSet(uint16_t ColumnStart, uint16_t ColumnEnd);
static void GC9D01_RowSet(uint16_t RowStart, uint16_t RowEnd);
static void SwapInt16Values(int16_t *pValue1, int16_t *pValue2);
static void GC9D01_DrawLine_Slow(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);

	
//##############################################################################
	  
	  
//==============================================================================
	  
	  
	  
//==============================================================================
// 显示屏初始化过程
//==============================================================================
/* GC9D01 初始化序列表：**每个子数组的第 0 字节是命令码，第 1 字节是本条命令的参数个数**，
 * 之后是参数；不足一行的补 0（补的部分不会被发送）。
 *
 * 为什么用二维表而不是逐次 SendCmd/SendData：
 *  - 原实现是一条命令 + 每个参数各启动一次 DMA（本表原为 228 次事务），每次都要重配 DMA 并等任务通知；
 *  - 现在一条命令只发两笔：命令码 1 字节（D/C=0），全部参数一次 DMA（D/C=1），合计 129 次；
 *  - 参数直接长在表里，**不需要额外的拼接缓冲区，也不需要多字节联合拷贝**；
 *  - 表是 const，放 Flash，不占 RAM。
 *
 * 行格式：{ 命令码, 参数个数, 参数0, 参数1, ... }
 */
#define GC9D01_SEQ_MAX_PARAM 32U
#define GC9D01_SEQ_WIDTH     (GC9D01_SEQ_MAX_PARAM + 2U)

static const uint8_t gc9d01_init_seqa[][GC9D01_SEQ_WIDTH] = {
    { 0xFE, 0 },                          /* --- init ------------ */
    { 0xEF, 0 },                          
    { 0x80, 1, 0xFF },                    /* 打开内部寄存器 80~8Fh ----- */
    { 0x81, 1, 0xFF },                    
    { 0x82, 1, 0xFF },                    
    { 0x83, 1, 0xFF },                    
    { 0x84, 1, 0xFF },                    
    { 0x85, 1, 0xFF },                    
    { 0x86, 1, 0xFF },                    
    { 0x87, 1, 0xFF },                    
    { 0x88, 1, 0xFF },                    
    { 0x89, 1, 0xFF },                    
    { 0x8A, 1, 0xFF },                    
    { 0x8B, 1, 0xFF },                    
    { 0x8C, 1, 0xFF },                    
    { 0x8D, 1, 0xFF },                    
    { 0x8E, 1, 0xFF },                    
    { 0x8F, 1, 0xFF },                    
    { 0xEC, 1, 0x11 },                    /* -------------------------------------- / 设置翻转（Inversion）模式 */
    { 0x7E, 1, 0x7a },                    /* VGL 大小 */
    { 0x74, 7, 0x02, 0x0E, 0x00, 0x00, 0x28, 0x00, 0x00 },/* 修改帧率 */
    { 0x98, 1, 0x3E },                    /* 内部电压调节 */
    { 0x99, 1, 0x3E },                    
    { 0xB5, 2, 0x0E, 0x0E },              /* Blanking Porch Control (B5h) VFP=14 VBP=14 HBP=Off */
    { 0x60, 4, 0x38, 0x09, 0x6D, 0x67 },  /* gip 时序起点 */
    { 0x63, 5, 0x38, 0xAD, 0x6D, 0x67, 0x05 },
    { 0x64, 6, 0x38, 0x0B, 0x70, 0xAB, 0x6D, 0x67 },
    { 0x66, 6, 0x38, 0x0F, 0x70, 0xAF, 0x6d, 0x67 },
    { 0x6A, 2, 0x00, 0x00 },              
    { 0x68, 7, 0x3B, 0x08, 0x04, 0x00, 0x04, 0x64, 0x67 },
    { 0x6C, 7, 0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50 },
    { 0x6E, 32, 0x00, 0x00, 0x00, 0x00, 0x07, 0x01, 0x13, 0x11, 0x0B, 0x09, 0x16, 0x15, 0x1D, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x1D, 0x15, 0x16, 0x0A, 0x0C, 0x12, 0x14, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00 },
    { 0xA9, 1, 0x1B },                    /* gip 时序终点 / 内部电压配置开始 */
    { 0xA8, 1, 0x6B },                    
    { 0xA8, 1, 0x6D },                    
    { 0xA7, 1, 0x40 },                    
    { 0xAD, 1, 0x47 },                    
    { 0xAF, 1, 0x73 },                    
    { 0xAF, 1, 0x73 },                    
    { 0xAC, 1, 0x44 },                    
    { 0xA3, 1, 0x6C },                    
    { 0xCB, 1, 0x00 },                    
    { 0xCD, 1, 0x22 },                    
    { 0xC2, 1, 0x10 },                    
    { 0xC5, 1, 0x00 },                    
    { 0xC6, 1, 0x0E },                    
    { 0xC7, 1, 0x1f },                    
    { 0xC8, 1, 0x0E },                    
    { 0xbf, 1, 0x00 },                    /* Dual-Single Gate Select (BFh) 0=>Single gate */
    { 0xF9, 1, 0x20 },                    /* 与 SOU 相关的校正 */
    { 0x9b, 1, 0x3b },                    /* 调节 vreg 电压 */
    { 0x93, 3, 0x33, 0x7f, 0x00 },        
    { 0x70, 6, 0x0E, 0x0f, 0x03, 0x0e, 0x0f, 0x03 },/* 调节 VGH/VGL CLK 70，71h */
    { 0x71, 3, 0x0e, 0x16, 0x03 },        
    { 0x91, 2, 0x0e, 0x09 },              /* 内部电压调节 */
    { 0xc3, 1, 0x2c },                    /* 调节 vreg 电压 */
    { 0xc4, 1, 0x1a },                    
    { 0xf0, 6, 0x51, 0x13, 0x0c, 0x06, 0x00, 0x2f },/* SET_GAMMA1 (F0h) */
    { 0xf2, 6, 0x51, 0x13, 0x0c, 0x06, 0x00, 0x33 },/* SET_GAMMA3 (F2h) */
    { 0xf1, 6, 0x3c, 0x94, 0x4f, 0x33, 0x34, 0xCf },/* SET_GAMMA2 (F1h) */
    { 0xf3, 6, 0x4d, 0x94, 0x4f, 0x33, 0x34, 0xCf },/* SET_GAMMA4 (F3h) */
    { GC9D01_DisplayFunctionControl, 2, 0x00, 0x10 },/* Scan direction */
    { GC9D01_MADCTL, 1, GC9D01_ROTATION },/* def rotation */
    { GC9D01_COLMOD, 1, ColorMode_MCU_16bit & 0x77 },/* ColorMode */
    { 0x11, 0 },                          /* Sleep Out Mode (11h) and delay(200) */
};

/* 收尾段：只发 Memory Write(0x2C)。
 * Display ON(0x29) 单独由下面的条件决定发不发——它决定"面板什么时候开始把 GRAM
 * 锁存成显示帧"，必须和"帧是否已经写好"配合，否则会闪一帧睡前旧画面。 */
static const uint8_t gc9d01_init_seqb[][GC9D01_SEQ_WIDTH] = {
    { 0x2C, 0 },                          /* Memory Write (2Ch) D=0 */
};

/* 把一行表项发出去：先命令（D/C=0），再本命令的全部参数（D/C=1，一次 DMA）。
 * GSPI_Transmit 的形参不是 const，这里显式去 const，表本身不会被修改。 */
static void GC9D01_SendSeq(const uint8_t (*seq)[GC9D01_SEQ_WIDTH], uint16_t rows)
{
    uint16_t i;
    uint8_t  n;

    for (i = 0U; i < rows; i++) {
        n = seq[i][1];              /* 本行参数个数 */

        /* 命令阶段：D/C = 0 */
        gpio_bits_reset(DC_GPIO_PORT, DC_PIN);
        GSPI_Transmit((uint8_t *)&seq[i][0], 1U);

        /* 数据阶段：D/C = 1，本命令的全部参数一次性发完 */
        if (n != 0U) {
            gpio_bits_set(DC_GPIO_PORT, DC_PIN);
            GSPI_Transmit((uint8_t *)&seq[i][2], (size_t)n);
        }
    }

    /* 让 D/C 回到数据态，与旧实现（SendCmd 末尾把 DC 拉高）保持一致 */
    gpio_bits_set(DC_GPIO_PORT, DC_PIN);
}

#define GC9D01_SEQ_ROWS(t)  ((uint16_t)(sizeof(t) / sizeof((t)[0])))

/* ── LCD 供电开关：PA3~PA7 五个 GPIO 并联（2026-10-27 固化）──────────────────
 * 旧版电路里 PA3(WP)/PA4~PA7 是给 SPI Flash 用的，Flash 已拆、这些脚悬空，
 * 因此把 LCD 的 VDD 接到这五个脚上，相当于一个"用 IO 做的负载开关"：
 * 五脚同开同关分摊电流（亮屏时整条 LCD 分支约 2mA@7.85V 输入侧）。
 *
 * 时序（实测）：
 *   上电：先写 ODR=高 → 再切输出模式（避免切换瞬间的低电平毛刺）→ 等 5ms 供电稳定；
 *   下电：直接写 ODR=低 → 切输出模式，**不需要**额外等待。
 * 注意：下电之前必须先把 SPI/RST/DC 置高阻（pm_sleep_pins_idle() 已按此顺序做），
 *       否则那些脚会经输入保护结构反向给屏供电。 */
void GC9D01_PowerRailSet(uint8_t on)
{
    gpio_init_type g;
    uint16_t pins;

    pins = (uint16_t)(GPIO_PINS_3 | GPIO_PINS_4 | GPIO_PINS_5 | GPIO_PINS_6 | GPIO_PINS_7);

    gpio_default_para_init(&g);
    g.gpio_pins = pins;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_pull = GPIO_PULL_NONE;
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;   /* 供电脚用强驱动 */

    if (on != 0U) {
        gpio_bits_set(GPIOA, pins);
        gpio_init(GPIOA, &g);
        /* 上电后等电压稳定再让调用者发复位/初始化命令 */
        wk_delay_ms(GC9D01_PWR_SETTLE_MS);
    } else {
        gpio_bits_reset(GPIOA, pins);
        gpio_init(GPIOA, &g);
    }
}

void GC9D01_Init(void){
    /* 面板几何：本工程固定 40x160，横放由旋转配置决定 */
    GC9D01_Width  = GC9D01_WIDTH;
    GC9D01_Height = GC9D01_HEIGHT;

    /* ① 硬件复位（RST 拉低 GC9D01_RESET_LOW_MS 后释放） */
    GC9D01_HardReset();

    /* ② 复位释放后的稳定时间（GC9D01_INIT_SETTLE_MS） */
    vTaskDelay(GC9D01_INIT_SETTLE_MS);

    /* ③ 寄存器配置段 1：内部寄存器解锁 → 电压/GIP/Gamma → Sleep Out(0x11) */
    GC9D01_SendSeq(gc9d01_init_seqa, GC9D01_SEQ_ROWS(gc9d01_init_seqa));

    /* ④ Sleep Out 后等显示模拟电路稳定（GC9D01_INIT_SLPOUT_MS） */
    vTaskDelay(GC9D01_INIT_SLPOUT_MS);

    /* ⑤ 收尾：Memory Write(0x2C) */
    GC9D01_SendSeq(gc9d01_init_seqb, GC9D01_SEQ_ROWS(gc9d01_init_seqb));

    /* ⑥ Display On(0x29) 这里**故意不发**：把"开始输出"推迟到 ui_task
     *   把首帧写进 GRAM 之后（见 ui_task 的显示输出门闩）。
     *   否则面板会在 DISPON 后就着旧 GRAM 先输出一帧，表现为"偶发闪一下旧画面"。 */
}
//==============================================================================

/* ── 通用的命令/数据发送（非初始化序列的散点写：SetWindow、DisplayPower、SleepIn/Out 等）──
 * 初始化序列已改为走上面的二维表 GC9D01_SendSeq()，一条命令只发两笔；
 * 这里保留最朴素的形式：命令 1 字节（D/C=0）、数据 1 字节（D/C=1）。 */
__inline static void GC9D01_SendCmd(uint8_t Cmd){
    gpio_bits_reset(DC_GPIO_PORT, DC_PIN);
    GSPI_Transmit(&Cmd, 1);
    gpio_bits_set(DC_GPIO_PORT, DC_PIN);   /* 之后默认处于数据态，与历史行为一致 */
}

__inline static void GC9D01_SendData(uint8_t Data ){
    gpio_bits_set(DC_GPIO_PORT, DC_PIN);
    GSPI_Transmit(&Data, 1);
}

__inline static void GC9D01_SendDataMASS(uint8_t* buff, size_t buff_size){
    gpio_bits_set(DC_GPIO_PORT, DC_PIN);
    GSPI_Transmit(buff, buff_size);}

void GC9D01_HardReset(void){
	gpio_bits_reset(SRST_GPIO_PORT, SRST_PIN);
	vTaskDelay(GC9D01_RESET_LOW_MS);
	gpio_bits_set(SRST_GPIO_PORT, SRST_PIN);
}



void GC9D01_SleepModeEnter( void ){
	GC9D01_SendCmd(GC9D01_SLPIN);
	vTaskDelay(GC9D01_SLPIN_MS);
}

void GC9D01_SleepModeExit( void ){
	GC9D01_SendCmd(GC9D01_SLPOUT);
	vTaskDelay(GC9D01_SLPOUT_MS);
}

void GC9D01_DisplayPower(uint8_t On){
  if (On){
    GC9D01_SendCmd(GC9D01_DISPON);
  }
  else{
    GC9D01_SendCmd(GC9D01_DISPOFF);
  }
}

void GC9D01_InversionMode(uint8_t Mode){
  if (Mode){
    GC9D01_SendCmd(GC9D01_INVON);
  }
  else{
    GC9D01_SendCmd(GC9D01_INVOFF);
  }
}

void GC9D01_RamWrite(uint8_t *pBuff, uint32_t Len){
  GC9D01_SendDataMASS(pBuff, Len);
}

void GC9D01_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1){	
	
	GC9D01_ColumnSet(x0, x1);
	GC9D01_RowSet(y0, y1);
	
	// write to RAM
	GC9D01_SendCmd(GC9D01_RAMWR);
}

//==============================================================================
// 设置列起始地址与结束地址
//==============================================================================
static void GC9D01_ColumnSet(uint16_t ColumnStart, uint16_t ColumnEnd){
	
  if (ColumnStart > ColumnEnd){
    return;
  }
  
  if (ColumnEnd > GC9D01_Width){
    return;
  }
	
  ColumnStart += GC9D01_X_Start;
  ColumnEnd += GC9D01_X_Start;
  
  GC9D01_SendCmd(GC9D01_CASET);
  GC9D01_SendData(ColumnStart >> 8);  
  GC9D01_SendData(ColumnStart & 0xFF);  
  GC9D01_SendData(ColumnEnd >> 8);  
  GC9D01_SendData(ColumnEnd & 0xFF);  
  
}
//==============================================================================


//==============================================================================
// 设置行起始地址与结束地址
//==============================================================================
static void GC9D01_RowSet(uint16_t RowStart, uint16_t RowEnd){
	
  if (RowStart > RowEnd){
    return;
  }
  
  if (RowEnd > GC9D01_Height){
    return;
  }
  
  RowStart += GC9D01_Y_Start;
  RowEnd += GC9D01_Y_Start;
 
  GC9D01_SendCmd(GC9D01_RASET);
  GC9D01_SendData(RowStart >> 8);  
  GC9D01_SendData(RowStart & 0xFF);  
  GC9D01_SendData(RowEnd >> 8);  
  GC9D01_SendData(RowEnd & 0xFF);  

}
//==============================================================================


//==============================================================================
// 背光亮度控制过程（PWM）— TMR1 CH4 (PA11)
// 电路为 PWM 下拉点亮 LED，因此比较值取反：0 = 最暗, 255 = 最亮
//==============================================================================
void GC9D01_SetBL(uint8_t Value){
    uint16_t compare;

    /* Value 0~255 映射到 TMR1 CH4 比较值 1023~3 (取反)
     * Value=0 → 1023 (几乎恒高, LED 灭)
     * Value=255 → 3 (几乎恒低, LED 最亮) */
    compare = 1023U - ((uint16_t)Value * 4U);
    if (compare > 1023U) compare = 0U;
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_4, compare);
}
//==============================================================================


//==============================================================================
// 显示屏旋转（方向）设置过程
//==============================================================================
// 默认 1 号模式（共 1、2、3、4）
void GC9D01_rotation( uint8_t rotation ){
	GC9D01_SendCmd(GC9D01_MADCTL);

	// 上游驱动按 160 x 160（最大尺寸）设计
	// 换用其他尺寸时需靠 X_Start/Y_Start 抵消像素差

	  switch (rotation) {
		
		case 1:
			//== 0.99" 40 x 160 GC9D01 =================================================
			#ifdef GC9D01_IS_40X160
				GC9D01_SendData( GC9D01_MADCTL_MV | GC9D01_MADCTL_ML | GC9D01_MADCTL_RGB );
				GC9D01_Width = 160;
				GC9D01_Height = 40;
				GC9D01_X_Start = -60;
				GC9D01_Y_Start = 60;
				//GC9D01_FillScreen(0);
			#endif
			//==========================================================================

		 break;
		
		case 2:
			//== 0.99" 40 x 160 GC9D01 =================================================
			#ifdef GC9D01_IS_40X160
				GC9D01_SendData( GC9D01_MADCTL_RGB );
				GC9D01_Width = 40;
				GC9D01_Height = 160;
				GC9D01_X_Start = 0;
				GC9D01_Y_Start = 0;
				//GC9D01_FillScreen(0);
			#endif
			//==========================================================================
	
		 break;
		
	   case 3:
		   //== 0.99" 40 x 160 GC9D01 =================================================
			#ifdef GC9D01_IS_40X160
				GC9D01_SendData( GC9D01_MADCTL_MV | GC9D01_MADCTL_MX | GC9D01_MADCTL_MY | GC9D01_MADCTL_ML | GC9D01_MADCTL_RGB );
				GC9D01_Width = 160;
				GC9D01_Height = 40;
				GC9D01_X_Start = -60;
				GC9D01_Y_Start = 60;
				//GC9D01_FillScreen(0);
			#endif
			//==========================================================================
			
		 break;
	   
	   case 4:
		   //== 0.99" 40 x 160 GC9D01 =================================================
			#ifdef GC9D01_IS_40X160
				GC9D01_SendData(GC9D01_MADCTL_MX | GC9D01_MADCTL_MY | GC9D01_MADCTL_RGB);
				GC9D01_Width = 40;
				GC9D01_Height = 160;
				GC9D01_X_Start = 0;
				GC9D01_Y_Start = 0;
				//GC9D01_FillScreen(0);
			#endif
			//==========================================================================
	   
		 break;
	   
	   default:
		 break;
	  }
}
//==============================================================================


//#########################################################################################################################
//#########################################################################################################################


/************************ (C) COPYRIGHT GKP *****END OF FILE****/
