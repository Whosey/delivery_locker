#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

/*
 * ============================================================
 * 外卖防盗系统：HX711真实称重版
 * ============================================================
 *
 * 当前接线方案：
 *
 * HX711 VCC       -> 星火1号 3.3V
 * HX711 GND       -> 星火1号 GND
 * HX711 DT/DOUT   -> 40Pin 第11脚 GPIO_PA1
 * HX711 SCK/CLK   -> 40Pin 第12脚 GPIO_PA0
 *
 * 使用流程：
 *
 * 1. 接好 HX711
 * 2. 下载程序
 * 3. 打开串口终端，波特率 115200
 * 4. 输入 hx711_raw，确认 raw 有数值
 * 5. 空载输入 hx711_tare
 * 6. 放已知重量，例如 500g，输入 hx711_cal 500
 * 7. 输入 hx711_weight 查看重量
 * 8. 放上外卖，输入 guard_start
 * 9. 拿走外卖，系统报警
 */

/* ===================== HX711 引脚配置 ===================== */

/*
 * HX711 DT/DOUT -> PA1
 * HX711 SCK/CLK -> PA0
 */
#define HX711_DOUT_PIN              GET_PIN(A, 1)
#define HX711_SCK_PIN               GET_PIN(A, 0)

/* ===================== 蜂鸣器配置 ===================== */

/*
 * 0：暂时不使用真实蜂鸣器，只在串口打印报警信息
 * 1：使用 GPIO 控制蜂鸣器
 *
 * 如果你后面接蜂鸣器，再把 USE_BUZZER 改成 1。
 */
#define USE_BUZZER                  1

/*
 * 如果你蜂鸣器接到 PA5，就保持下面这样。
 * 如果接到别的引脚，例如 PB0，就改成 GET_PIN(B, 0)。
 */

#define BUZZER_PIN                  GET_PIN(A, 5)
/*
 * 用户确认超时时间。
 * 重量异常后，如果 15 秒内用户没有选择“是”，自动进入正式报警。
 */
#define APP_CONFIRM_TIMEOUT_MS      15000
/*
 * 摄像头触发输出：
 *
 * STM32 PMOD_PA4 -> ESP32-CAM IO16
 *
 * 报警时：
 *     PA4 输出高电平
 *
 * 报警解除时：
 *     PA4 输出低电平
 */
#define USE_CAMERA_TRIGGER          1
#define CAMERA_TRIGGER_PIN          GET_PIN(A, 4)

/* ===================== 防盗参数配置 ===================== */

/*
 * MIN_VALID_WEIGHT_G：
 *     小于这个重量，认为平台上基本没有外卖。
 *
 * DEFAULT_DROP_THRESHOLD_G：
 *     当前重量比初始重量下降超过这个值，就认为外卖被拿走。
 *
 * SAMPLE_PERIOD_MS：
 *     每隔多少毫秒检测一次重量。
 */
#define MIN_VALID_WEIGHT_G          100
#define DEFAULT_DROP_THRESHOLD_G    300
#define SAMPLE_PERIOD_MS            500
/*
 * RETURN_RISE_THRESHOLD_G：
 *     报警之后，重量至少比报警瞬间上升多少克，才认为“可能放回来了”。
 *
 * RETURN_NEAR_ORIGINAL_G：
 *     放回后的重量与原始外卖重量允许相差多少克。
 *     例如原来是 500g，允许差 150g，则 350g~650g 都认为接近原重量。
 *
 * RETURN_CONFIRM_COUNT：
 *     连续检测多少次都满足“放回条件”，才真正关闭蜂鸣器。
 *     这样可以防止传感器瞬间跳变导致误关闭。
 */
#define RETURN_RISE_THRESHOLD_G     200
#define RETURN_NEAR_ORIGINAL_G      150
#define RETURN_CONFIRM_COUNT        3
/*
 * HX711 读取错误标记。
 */
#define HX711_READ_ERROR            0x7FFFFFFF

typedef enum
{
    GUARD_STATE_IDLE = 0,
    GUARD_STATE_MONITORING,
    GUARD_STATE_ALARM_PENDING,
    GUARD_STATE_ALARM_ACTIVE
} guard_state_t;
/* ===================== HX711 校准变量 ===================== */

/*
 * g_hx711_offset：
 *     空载时的 HX711 原始值，也就是去皮值。
 *
 * g_hx711_scale_x1000：
 *     校准系数，单位是 raw_count/g * 1000。
 *     用整数保存，避免使用 float。
 *
 * g_hx711_calibrated：
 *     是否已经完成校准。
 */
static int32_t g_hx711_offset = 0;
static int32_t g_hx711_scale_x1000 = 0;
static int g_hx711_calibrated = 0;

/* ===================== 防盗状态变量 ===================== */

/*
 * 重量统一使用 g_x100 保存。
 *
 * 例如：
 *     850.25g 保存为 85025
 *     100.00g 保存为 10000
 */
static int32_t g_original_weight_g_x100 = 0;
static int32_t g_drop_threshold_g = DEFAULT_DROP_THRESHOLD_G;
static guard_state_t g_state = GUARD_STATE_IDLE;
static int g_alarm_on = 0;
static int32_t g_latest_weight_g_x100 = 0;
/*
 * 进入疑似报警时的重量。
 */
static int32_t g_alarm_weight_g_x100 = 0;

/*
 * 放回物品自动恢复时用。
 */
static int g_return_ok_count = 0;

/*
 * 进入 ALARM_PENDING 的时间。
 * 用来做 15 秒超时自动报警。
 */
static rt_tick_t g_pending_start_tick = 0;
/*
 * g_alarm_weight_g_x100：
 *     进入报警那一刻的重量。
 *     后面用 current_weight - g_alarm_weight 来判断重量是否重新上升。
 *
 * g_return_ok_count：
 *     连续满足“放回条件”的次数。
 */
/* ===================== 工具函数 ===================== */
static int32_t i32_abs(int32_t value)
{
    if (value < 0)
    {
        return -value;
    }

    return value;
}
static const char *state_to_string(guard_state_t state)
{
    switch (state)
    {
    case GUARD_STATE_IDLE:
        return "IDLE";

    case GUARD_STATE_MONITORING:
        return "MONITORING";

    case GUARD_STATE_ALARM_PENDING:
        return "ALARM_PENDING";

    case GUARD_STATE_ALARM_ACTIVE:
        return "ALARM_ACTIVE";

    default:
        return "UNKNOWN";
    }
}
/*
 * 打印保留 2 位小数的整数。
 *
 * 例如：
 *     85025 -> 850.25
 */
static void print_fixed_2(int32_t value_x100)
{
    if (value_x100 < 0)
    {
        rt_kprintf("-");
        value_x100 = -value_x100;
    }

    rt_kprintf("%d.%02d", value_x100 / 100, value_x100 % 100);
}

/*
 * 打印保留 3 位小数的整数。
 *
 * 例如：
 *     123456 -> 123.456
 */
static void print_fixed_3(int32_t value_x1000)
{
    if (value_x1000 < 0)
    {
        rt_kprintf("-");
        value_x1000 = -value_x1000;
    }

    rt_kprintf("%d.%03d", value_x1000 / 1000, value_x1000 % 1000);
}

/* ===================== HX711 底层驱动 ===================== */

static void hx711_init(void)
{
    rt_pin_mode(HX711_SCK_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(HX711_DOUT_PIN, PIN_MODE_INPUT);

    /*
     * HX711 的 SCK 默认必须保持低电平。
     * 如果 SCK 长时间为高电平，HX711 会进入掉电模式。
     */
    rt_pin_write(HX711_SCK_PIN, PIN_LOW);

    rt_thread_mdelay(100);
}

/*
 * 等待 HX711 数据准备好。
 *
 * HX711 的 DOUT 为低电平时，表示数据准备好了。
 */
static rt_err_t hx711_wait_ready(int32_t timeout_ms)
{
    rt_tick_t start_tick;
    rt_tick_t timeout_tick;

    start_tick = rt_tick_get();
    timeout_tick = rt_tick_from_millisecond(timeout_ms);

    while (rt_pin_read(HX711_DOUT_PIN) == PIN_HIGH)
    {
        if ((rt_tick_get() - start_tick) > timeout_tick)
        {
            return -RT_ETIMEOUT;
        }

        rt_thread_mdelay(1);
    }

    return RT_EOK;
}

/*
 * 读取 HX711 原始 24 位有符号数据。
 *
 * A 通道，增益 128：
 *     读取 24 位数据后，再给 1 个脉冲。
 */
static int32_t hx711_read_raw(void)
{
    int32_t data;
    int i;
    rt_base_t level;

    data = 0;

    if (hx711_wait_ready(1000) != RT_EOK)
    {
        return HX711_READ_ERROR;
    }

    level = rt_hw_interrupt_disable();

    for (i = 0; i < 24; i++)
    {
        rt_pin_write(HX711_SCK_PIN, PIN_HIGH);
        rt_hw_us_delay(1);

        data = data << 1;

        rt_pin_write(HX711_SCK_PIN, PIN_LOW);
        rt_hw_us_delay(1);

        if (rt_pin_read(HX711_DOUT_PIN) == PIN_HIGH)
        {
            data++;
        }
    }

    /*
     * 第 25 个脉冲：
     * 选择 A 通道，增益 128。
     */
    rt_pin_write(HX711_SCK_PIN, PIN_HIGH);
    rt_hw_us_delay(1);
    rt_pin_write(HX711_SCK_PIN, PIN_LOW);
    rt_hw_us_delay(1);

    rt_hw_interrupt_enable(level);

    /*
     * 24 位有符号数扩展成 32 位有符号数。
     */
    if (data & 0x800000)
    {
        data |= 0xFF000000;
    }

    return data;
}

/*
 * 多次读取求平均，减少抖动。
 */
static int32_t hx711_read_average(int times)
{
    int i;
    int valid_count;
    int64_t sum;

    valid_count = 0;
    sum = 0;

    if (times <= 0)
    {
        times = 1;
    }

    for (i = 0; i < times; i++)
    {
        int32_t value;

        value = hx711_read_raw();

        if (value != HX711_READ_ERROR)
        {
            sum += value;
            valid_count++;
        }

        rt_thread_mdelay(5);
    }

    if (valid_count == 0)
    {
        return HX711_READ_ERROR;
    }

    return (int32_t)(sum / valid_count);
}

/*
 * 获取当前重量。
 *
 * 返回值：
 *     RT_EOK       ：成功
 *     -RT_ERROR    ：还没有校准
 *     -RT_ETIMEOUT ：HX711 读取超时
 *
 * weight_g_x100：
 *     单位为 0.01g。
 *
 *     例如：
 *         850.25g -> 85025
 */
static rt_err_t hx711_get_weight_g_x100(int32_t *weight_g_x100)
{
    int32_t raw;
    int32_t net;
    int64_t temp;

    if (weight_g_x100 == RT_NULL)
    {
        return -RT_ERROR;
    }

    if (!g_hx711_calibrated || g_hx711_scale_x1000 == 0)
    {
        return -RT_ERROR;
    }

    raw = hx711_read_average(5);

    if (raw == HX711_READ_ERROR)
    {
        return -RT_ETIMEOUT;
    }

    net = raw - g_hx711_offset;

    /*
     * scale = raw_count / g
     *
     * g_hx711_scale_x1000 = scale * 1000
     *
     * weight_g = net / scale
     *
     * weight_g_x100 = weight_g * 100
     *               = net * 100 / scale
     *               = net * 100 * 1000 / scale_x1000
     */
    temp = (int64_t)net * 100000;
    *weight_g_x100 = (int32_t)(temp / g_hx711_scale_x1000);

    return RT_EOK;
}

/* ===================== 报警控制 ===================== */
static void alarm_set(int on)
{
    if (on)
    {
        if (!g_alarm_on)
        {
            g_alarm_on = 1;
            rt_kprintf("[ALARM] ON: Food may be removed!\n");
        }

#if USE_BUZZER
        rt_pin_write(BUZZER_PIN, PIN_HIGH);
#endif

#if USE_CAMERA_TRIGGER
        rt_pin_write(CAMERA_TRIGGER_PIN, PIN_HIGH);
#endif
    }
    else
    {
        if (g_alarm_on)
        {
            g_alarm_on = 0;
            rt_kprintf("[ALARM] OFF\n");
        }

#if USE_BUZZER
        rt_pin_write(BUZZER_PIN, PIN_LOW);
#endif

#if USE_CAMERA_TRIGGER
        rt_pin_write(CAMERA_TRIGGER_PIN, PIN_LOW);
#endif
    }
}

/* ===================== 状态打印 ===================== */

static void print_guard_status(void)
{
    int32_t current_weight_g_x100;
    int32_t drop_g_x100;
    rt_err_t ret;

    current_weight_g_x100 = 0;
    drop_g_x100 = 0;

    ret = hx711_get_weight_g_x100(&current_weight_g_x100);

    if (ret == RT_EOK)
    {
        drop_g_x100 = g_original_weight_g_x100 - current_weight_g_x100;
    }

    rt_kprintf("\n");
    rt_kprintf("=============== SYSTEM STATUS ===============\n");
    rt_kprintf("state              : %s\n", state_to_string(g_state));
    rt_kprintf("calibrated          : %s\n", g_hx711_calibrated ? "YES" : "NO");
    rt_kprintf("offset raw          : %d\n", g_hx711_offset);

    rt_kprintf("scale               : ");
    print_fixed_3(g_hx711_scale_x1000);
    rt_kprintf(" raw_count/g\n");

    if (ret == RT_EOK)
    {
        rt_kprintf("current weight      : ");
        print_fixed_2(current_weight_g_x100);
        rt_kprintf(" g\n");

        rt_kprintf("original weight     : ");
        print_fixed_2(g_original_weight_g_x100);
        rt_kprintf(" g\n");

        rt_kprintf("drop weight         : ");
        print_fixed_2(drop_g_x100);
        rt_kprintf(" g\n");
    }
    else if (ret == -RT_ERROR)
    {
        rt_kprintf("current weight      : not calibrated\n");
    }
    else
    {
        rt_kprintf("current weight      : HX711 timeout\n");
    }

    rt_kprintf("drop threshold      : %d g\n", g_drop_threshold_g);
    rt_kprintf("min valid weight    : %d g\n", MIN_VALID_WEIGHT_G);
    rt_kprintf("alarm               : %s\n", g_alarm_on ? "ON" : "OFF");
    rt_kprintf("=============================================\n");
}

/* ===================== 防盗逻辑 ===================== */
static void print_x100_number(int32_t value)
{
    if (value < 0)
    {
        rt_kprintf("-");
        value = -value;
    }

    rt_kprintf("%d.%02d", value / 100, value % 100);
}

static void enter_alarm_pending(int32_t current_weight_g_x100,
                                int32_t drop_g_x100,
                                const char *reason)
{
    g_alarm_weight_g_x100 = current_weight_g_x100;
    g_return_ok_count = 0;
    g_pending_start_tick = rt_tick_get();

    /*
     * 进入待确认状态。
     * 注意：此时不是正式报警，所以不能让蜂鸣器响，也不能让摄像头拍。
     */
    g_state = GUARD_STATE_ALARM_PENDING;
    alarm_set(0);

    rt_kprintf("[PENDING] Weight abnormal, waiting for app confirm.\n");

    /*
     * 给网页前端用的 JSON。
     * 前端收到这个 JSON 后会弹窗。
     */
    rt_kprintf("{\"type\":\"ALARM_PENDING\",");
    rt_kprintf("\"reason\":\"%s\",", reason);

    rt_kprintf("\"weightG\":");
    print_x100_number(current_weight_g_x100);
    rt_kprintf(",");

    rt_kprintf("\"originalWeightG\":");
    print_x100_number(g_original_weight_g_x100);
    rt_kprintf(",");

    rt_kprintf("\"dropG\":");
    print_x100_number(drop_g_x100);
    rt_kprintf(",");

    rt_kprintf("\"timeout\":15}\n");
}

static void enter_alarm_active(const char *reason)
{
    g_state = GUARD_STATE_ALARM_ACTIVE;

    rt_kprintf("[APP] Alarm confirmed: %s\n", reason);

    /*
     * 正式报警：
     * 蜂鸣器响
     * 摄像头开始拍照
     */
    alarm_set(1);

    rt_kprintf("{\"type\":\"EVENT\",\"event\":\"ALARM_ACTIVE\",\"reason\":\"%s\"}\n", reason);
}

static void guard_logic_update(void)
{
    static int print_counter = 0;

    int32_t current_weight_g_x100;
    int32_t drop_g_x100;
    int32_t rise_from_alarm_g_x100;
    int32_t diff_from_original_g_x100;
    rt_err_t ret;

    if (g_state == GUARD_STATE_IDLE)
    {
        return;
    }

    ret = hx711_get_weight_g_x100(&current_weight_g_x100);

    if (ret != RT_EOK)
    {
        if (ret == -RT_ERROR)
        {
            rt_kprintf("[ERROR] HX711 not calibrated. Use hx711_tare and hx711_cal first.\n");
        }
        else
        {
            rt_kprintf("[ERROR] HX711 read timeout. Check wiring.\n");
        }

        return;
    }

    g_latest_weight_g_x100 = current_weight_g_x100;

    drop_g_x100 = g_original_weight_g_x100 - current_weight_g_x100;

    /*
     * ============================================================
     * 状态：等待用户在网页端确认
     * ============================================================
     *
     * 进入 ALARM_PENDING 后，蜂鸣器和摄像头先不启动。
     * 如果用户 15 秒内没有点击“是”，就自动进入正式报警。
     */
    if (g_state == GUARD_STATE_ALARM_PENDING)
    {
        rt_tick_t now = rt_tick_get();
        rt_tick_t elapsed = now - g_pending_start_tick;

        if (elapsed >= rt_tick_from_millisecond(APP_CONFIRM_TIMEOUT_MS))
        {
            rt_kprintf("[PENDING] App confirm timeout. Alarm active automatically.\n");
            enter_alarm_active("APP_TIMEOUT");
        }

        return;
    }


    if (g_state == GUARD_STATE_MONITORING)
    {
        /*
         * 情况 1：
         * 当前重量太低，认为外卖基本被拿走。
         */
        if (current_weight_g_x100 < MIN_VALID_WEIGHT_G * 100)
        {
            rt_kprintf("[WARN] Current weight too low: ");
            print_fixed_2(current_weight_g_x100);
            rt_kprintf(" g\n");

            enter_alarm_pending(current_weight_g_x100, drop_g_x100, "WEIGHT_TOO_LOW");
            return;
        }

        /*
         * 情况 2：
         * 重量下降超过阈值，认为外卖被拿走或被拿走一部分。
         */
        if (drop_g_x100 > g_drop_threshold_g * 100)
        {
            rt_kprintf("[WARN] Weight dropped too much: ");
            print_fixed_2(drop_g_x100);
            rt_kprintf(" g\n");

            enter_alarm_pending(current_weight_g_x100, drop_g_x100, "DROP_TOO_MUCH");
            return;
        }

        print_counter++;

        if (print_counter >= 2)
        {
            print_counter = 0;

            rt_kprintf("[OK] weight=");
            print_fixed_2(current_weight_g_x100);
            rt_kprintf(" g, original=");
            print_fixed_2(g_original_weight_g_x100);
            rt_kprintf(" g, drop=");
            print_fixed_2(drop_g_x100);
            rt_kprintf(" g, state=%s\n", state_to_string(g_state));
        }
    }
    /*
     * ============================================================
     * 状态 2：已经报警
     * ============================================================
     *
     * 这里新增自动关闭蜂鸣器逻辑：
     *
     * 1. 当前重量比报警瞬间重量上升超过 RETURN_RISE_THRESHOLD_G
     * 2. 当前重量接近原始外卖重量
     * 3. 连续 RETURN_CONFIRM_COUNT 次满足条件
     *
     * 满足后：
     *     关闭蜂鸣器
     *     回到 MONITORING 状态
     *     把当前重量重新记录为新的 original weight
     */
    else if (g_state == GUARD_STATE_ALARM_ACTIVE)
    {
        rise_from_alarm_g_x100 = current_weight_g_x100 - g_alarm_weight_g_x100;
        diff_from_original_g_x100 = i32_abs(current_weight_g_x100 - g_original_weight_g_x100);

        /*
         * 判断物品是否已经放回。
         */
        if ((rise_from_alarm_g_x100 >= RETURN_RISE_THRESHOLD_G * 100) &&
            (diff_from_original_g_x100 <= RETURN_NEAR_ORIGINAL_G * 100))
        {
            g_return_ok_count++;

            rt_kprintf("[RETURN CHECK] weight=");
            print_fixed_2(current_weight_g_x100);
            rt_kprintf(" g, rise=");
            print_fixed_2(rise_from_alarm_g_x100);
            rt_kprintf(" g, diff_from_original=");
            print_fixed_2(diff_from_original_g_x100);
            rt_kprintf(" g, count=%d/%d\n", g_return_ok_count, RETURN_CONFIRM_COUNT);

            if (g_return_ok_count >= RETURN_CONFIRM_COUNT)
            {
                /*
                 * 连续多次确认物品放回，关闭蜂鸣器。
                 */
                rt_kprintf("[RECOVER] Item returned. Alarm will be turned off automatically.\n");

                alarm_set(0);

                /*
                 * 回到监测状态。
                 */
                g_state = GUARD_STATE_MONITORING;

                /*
                 * 把当前重量作为新的基准重量。
                 * 这样放回后如果重量略有误差，不会立刻再次报警。
                 */
                g_original_weight_g_x100 = current_weight_g_x100;

                /*
                 * 清空报警相关记录。
                 */
                g_alarm_weight_g_x100 = 0;
                g_return_ok_count = 0;

                rt_kprintf("[RECOVER] New original weight = ");
                print_fixed_2(g_original_weight_g_x100);
                rt_kprintf(" g, state=%s\n", state_to_string(g_state));

                return;
            }
        }
        else
        {
            /*
             * 如果某一次不满足放回条件，重新计数。
             */
            g_return_ok_count = 0;
        }

        /*
         * 还没确认放回，就保持报警。
         */
        alarm_set(1);

        print_counter++;

        if (print_counter >= 2)
        {
            print_counter = 0;

            rt_kprintf("[ALARM] weight=");
            print_fixed_2(current_weight_g_x100);
            rt_kprintf(" g, original=");
            print_fixed_2(g_original_weight_g_x100);
            rt_kprintf(" g, drop=");
            print_fixed_2(drop_g_x100);
            rt_kprintf(" g, rise_from_alarm=");
            print_fixed_2(rise_from_alarm_g_x100);
            rt_kprintf(" g, return_count=%d\n", g_return_ok_count);
        }
    }
}


/* ===================== MSH 命令 ===================== */

#ifdef RT_USING_FINSH

/*
 * 命令：hx711_raw
 *
 * 作用：
 *     查看 HX711 原始值。
 *
 * 用途：
 *     先确认传感器有没有反应。
 */
int hx711_raw(int argc, char **argv)
{
    int32_t raw;
    int32_t net;

    raw = hx711_read_average(5);

    if (raw == HX711_READ_ERROR)
    {
        rt_kprintf("HX711 raw read timeout. Check VCC/GND/DT/SCK wiring.\n");
        return -1;
    }

    net = raw - g_hx711_offset;

    rt_kprintf("raw=%d, offset=%d, net=%d\n", raw, g_hx711_offset, net);

    return 0;
}
MSH_CMD_EXPORT(hx711_raw, show HX711 raw value);

/*
 * 命令：hx711_tare
 * 命令：hx711_tare 50
 *
 * 作用：
 *     空载去皮。
 *
 * 使用：
 *     平台上不要放东西，然后输入 hx711_tare。
 */
int hx711_tare(int argc, char **argv)
{
    int times;
    int32_t raw;

    times = 30;

    if (argc >= 2)
    {
        times = atoi(argv[1]);

        if (times <= 0)
        {
            times = 30;
        }
    }

    rt_kprintf("Taring... Please remove all load from platform.\n");

    raw = hx711_read_average(times);

    if (raw == HX711_READ_ERROR)
    {
        rt_kprintf("Tare failed: HX711 timeout. Check wiring.\n");
        return -1;
    }

    g_hx711_offset = raw;

    rt_kprintf("Tare OK. offset=%d\n", g_hx711_offset);

    return 0;
}
MSH_CMD_EXPORT(hx711_tare, tare HX711 zero point);

/*
 * 命令：hx711_cal 500
 *
 * 作用：
 *     用已知重量校准。
 *
 * 使用方法：
 *     1. 先空载，输入 hx711_tare
 *     2. 放上 500g 重物
 *     3. 输入 hx711_cal 500
 */
int hx711_cal(int argc, char **argv)
{
    int known_g;
    int32_t raw;
    int32_t net;
    int64_t scale_temp;

    if (argc < 2)
    {
        rt_kprintf("Usage: hx711_cal known_weight_g\n");
        rt_kprintf("Example: hx711_cal 500\n");
        return -1;
    }

    known_g = atoi(argv[1]);

    if (known_g <= 0)
    {
        rt_kprintf("known_weight_g must be greater than 0.\n");
        return -1;
    }

    rt_kprintf("Calibrating... Please put %d g weight on platform.\n", known_g);

    raw = hx711_read_average(30);

    if (raw == HX711_READ_ERROR)
    {
        rt_kprintf("Calibration failed: HX711 timeout. Check wiring.\n");
        return -1;
    }

    net = raw - g_hx711_offset;

    if (net > -100 && net < 100)
    {
        rt_kprintf("Calibration failed: net value too small: %d\n", net);
        rt_kprintf("Possible reasons:\n");
        rt_kprintf("1. No weight on platform.\n");
        rt_kprintf("2. Sensor wiring is wrong.\n");
        rt_kprintf("3. Load cell installation is stuck.\n");
        return -1;
    }

    /*
     * scale = net / known_g
     * 放大 1000 倍保存，避免使用 float。
     */
    scale_temp = (int64_t)net * 1000 / known_g;

    g_hx711_scale_x1000 = (int32_t)scale_temp;
    g_hx711_calibrated = 1;

    rt_kprintf("Calibration OK.\n");
    rt_kprintf("raw=%d, offset=%d, net=%d\n", raw, g_hx711_offset, net);

    rt_kprintf("scale=");
    print_fixed_3(g_hx711_scale_x1000);
    rt_kprintf(" raw_count/g\n");

    rt_kprintf("Note: calibration data is not saved after reset.\n");

    return 0;
}
MSH_CMD_EXPORT(hx711_cal, calibrate HX711 with known weight in gram);

/*
 * 命令：hx711_weight
 *
 * 作用：
 *     显示当前重量。
 */
int hx711_weight(int argc, char **argv)
{
    int32_t weight_g_x100;
    rt_err_t ret;

    ret = hx711_get_weight_g_x100(&weight_g_x100);

    if (ret == RT_EOK)
    {
        rt_kprintf("weight=");
        print_fixed_2(weight_g_x100);
        rt_kprintf(" g\n");
    }
    else if (ret == -RT_ERROR)
    {
        rt_kprintf("HX711 not calibrated. Use hx711_tare and hx711_cal first.\n");
    }
    else
    {
        rt_kprintf("HX711 read timeout. Check wiring.\n");
    }

    return 0;
}
MSH_CMD_EXPORT(hx711_weight, show current weight);

/*
 * 命令：guard_start
 *
 * 作用：
 *     放上外卖后，开始防盗监测。
 */
int guard_start(int argc, char **argv)
{
    int32_t current_weight_g_x100;
    rt_err_t ret;

    ret = hx711_get_weight_g_x100(&current_weight_g_x100);

    if (ret == -RT_ERROR)
    {
        rt_kprintf("Start failed: HX711 not calibrated.\n");
        rt_kprintf("Please run:\n");
        rt_kprintf("  hx711_tare\n");
        rt_kprintf("  hx711_cal 500\n");
        return -1;
    }

    if (ret != RT_EOK)
    {
        rt_kprintf("Start failed: HX711 timeout. Check wiring.\n");
        return -1;
    }

    if (current_weight_g_x100 < MIN_VALID_WEIGHT_G * 100)
    {
        rt_kprintf("[WARN] Current weight too low: ");
        print_fixed_2(current_weight_g_x100);
        rt_kprintf(" g\n");

        rt_kprintf("[ERROR] Food weight too low. Please put food on scale before guard_start.\n");
        return 0;
    }

    g_original_weight_g_x100 = current_weight_g_x100;
    g_alarm_weight_g_x100 = 0;
    g_return_ok_count = 0;
    g_state = GUARD_STATE_MONITORING;
    alarm_set(0);

    rt_kprintf("Guard started.\n");
    rt_kprintf("Original food weight = ");
    print_fixed_2(g_original_weight_g_x100);
    rt_kprintf(" g\n");
    rt_kprintf("Drop threshold = %d g\n", g_drop_threshold_g);

    return 0;
}
MSH_CMD_EXPORT(guard_start, start food anti-theft monitoring);

/*
 * 命令：guard_stop
 *
 * 作用：
 *     停止监测，关闭报警。
 */
int guard_stop(int argc, char **argv)
{
    g_state = GUARD_STATE_IDLE;
    g_original_weight_g_x100 = 0;
    g_alarm_weight_g_x100 = 0;
    g_return_ok_count = 0;
    alarm_set(0);

    rt_kprintf("Guard stopped. State is now IDLE.\n");

    return 0;
}
MSH_CMD_EXPORT(guard_stop, stop food anti-theft monitoring);

/*
 * 命令：guard_status
 *
 * 作用：
 *     查看当前系统状态。
 */
int guard_status(int argc, char **argv)
{
    int32_t current_weight_g_x100 = 0;
    int32_t drop_g_x100 = 0;
    rt_err_t ret;

    /*
     * 重点：
     * 无论当前是 IDLE / MONITORING / ALARM_PENDING / ALARM_ACTIVE，
     * 每次网页请求 guard_status 时，都主动读取一次 HX711。
     */
    ret = hx711_get_weight_g_x100(&current_weight_g_x100);

    if (ret == RT_EOK)
    {
        g_latest_weight_g_x100 = current_weight_g_x100;
    }
    else
    {
        /*
         * 如果读取失败，不要直接让网页崩。
         * 继续使用上一次的重量。
         */
        current_weight_g_x100 = g_latest_weight_g_x100;

        if (ret == -RT_ERROR)
        {
            rt_kprintf("[ERROR] HX711 not calibrated. Use hx711_tare and hx711_cal first.\n");
        }
        else
        {
            rt_kprintf("[ERROR] HX711 read timeout. Check wiring.\n");
        }
    }

    if (g_original_weight_g_x100 > 0)
    {
        drop_g_x100 = g_original_weight_g_x100 - g_latest_weight_g_x100;
    }
    else
    {
        drop_g_x100 = 0;
    }

    /*
     * 保留原来的文字状态输出。
     */
    print_guard_status();

    /*
     * 给网页解析用的 JSON 状态。
     */
    rt_kprintf("{\"type\":\"STATUS\",");
    rt_kprintf("\"state\":\"%s\",", state_to_string(g_state));

    rt_kprintf("\"weightG\":");
    print_x100_number(g_latest_weight_g_x100);
    rt_kprintf(",");

    rt_kprintf("\"originalWeightG\":");
    print_x100_number(g_original_weight_g_x100);
    rt_kprintf(",");

    rt_kprintf("\"dropG\":");
    print_x100_number(drop_g_x100);
    rt_kprintf(",");

    rt_kprintf("\"dropThresholdG\":%d,", g_drop_threshold_g);
    rt_kprintf("\"alarm\":%s,", g_alarm_on ? "true" : "false");

#if USE_CAMERA_TRIGGER
    rt_kprintf("\"camera\":%s,", g_alarm_on ? "true" : "false");
#else
    rt_kprintf("\"camera\":false,");
#endif

    rt_kprintf("\"calibrated\":%s}\n", ret == -RT_ERROR ? "false" : "true");

    return 0;
}
MSH_CMD_EXPORT(guard_status, show current guard status);


/*
 * 命令：alarm_off
 *
 * 作用：
 *     手动关闭报警，回到空闲状态。
 */
int alarm_off(int argc, char **argv)
{
    g_state = GUARD_STATE_IDLE;
    g_original_weight_g_x100 = 0;
    g_alarm_weight_g_x100 = 0;
    g_return_ok_count = 0;
    alarm_set(0);

    rt_kprintf("Alarm cleared. State is now IDLE.\n");

    return 0;
}
MSH_CMD_EXPORT(alarm_off, clear alarm manually);

static void app_yes(int argc, char **argv)
{
    if (g_state != GUARD_STATE_ALARM_PENDING)
    {
        rt_kprintf("[APP] app_yes ignored. Current state=%s\n", state_to_string(g_state));
        return;
    }

    /*
     * 用户确认是本人取餐：
     * 不启动蜂鸣器，不启动摄像头。
     */
    alarm_set(0);

    g_state = GUARD_STATE_IDLE;
    g_original_weight_g_x100 = 0;
    g_alarm_weight_g_x100 = 0;
    g_return_ok_count = 0;

    rt_kprintf("[APP] User confirmed self pickup. No alarm.\n");
    rt_kprintf("{\"type\":\"EVENT\",\"event\":\"SELF_CONFIRMED\"}\n");
}
MSH_CMD_EXPORT(app_yes, user confirmed self pickup);

static void app_no(int argc, char **argv)
{
    if (g_state != GUARD_STATE_ALARM_PENDING)
    {
        rt_kprintf("[APP] app_no ignored. Current state=%s\n", state_to_string(g_state));
        return;
    }

    /*
     * 用户确认不是本人：
     * 正式报警。
     */
    enter_alarm_active("USER_CONFIRMED_THEFT");
}
MSH_CMD_EXPORT(app_no, user confirmed not self pickup);

static void app_clear(int argc, char **argv)
{
    alarm_set(0);

    g_state = GUARD_STATE_IDLE;
    g_original_weight_g_x100 = 0;
    g_alarm_weight_g_x100 = 0;
    g_return_ok_count = 0;

    rt_kprintf("[APP] Alarm cleared by app.\n");
    rt_kprintf("{\"type\":\"EVENT\",\"event\":\"CLEAR\"}\n");
}
MSH_CMD_EXPORT(app_clear, clear alarm from app);

static void buzzer_on(int argc, char **argv)
{
#if USE_BUZZER
    rt_pin_write(BUZZER_PIN, PIN_HIGH);
#endif

    g_alarm_on = 1;
    rt_kprintf("[BUZZER] ON\n");
}
MSH_CMD_EXPORT(buzzer_on, turn buzzer on);

static void buzzer_off(int argc, char **argv)
{
#if USE_BUZZER
    rt_pin_write(BUZZER_PIN, PIN_LOW);
#endif

    g_alarm_on = 0;
    rt_kprintf("[BUZZER] OFF\n");
}
MSH_CMD_EXPORT(buzzer_off, turn buzzer off);

static void camera_start(int argc, char **argv)
{
#if USE_CAMERA_TRIGGER
    rt_pin_write(CAMERA_TRIGGER_PIN, PIN_HIGH);
#endif

    rt_kprintf("[CAMERA] START\n");
    rt_kprintf("{\"type\":\"EVENT\",\"event\":\"CAMERA_START\"}\n");
}
MSH_CMD_EXPORT(camera_start, start ESP32 camera recording);

static void camera_stop(int argc, char **argv)
{
#if USE_CAMERA_TRIGGER
    rt_pin_write(CAMERA_TRIGGER_PIN, PIN_LOW);
#endif

    rt_kprintf("[CAMERA] STOP\n");
    rt_kprintf("{\"type\":\"EVENT\",\"event\":\"CAMERA_STOP\"}\n");
}
MSH_CMD_EXPORT(camera_stop, stop ESP32 camera recording);



/*
 * 命令：set_drop 300
 *
 * 作用：
 *     设置重量下降报警阈值。
 *
 * 示例：
 *     set_drop 200
 *     set_drop 500
 */
int set_drop(int argc, char **argv)
{
    int value;

    if (argc < 2)
    {
        rt_kprintf("Usage: set_drop threshold_g\n");
        rt_kprintf("Example: set_drop 300\n");
        return -1;
    }

    value = atoi(argv[1]);

    if (value <= 0)
    {
        rt_kprintf("Threshold must be greater than 0.\n");
        return -1;
    }

    g_drop_threshold_g = value;

    rt_kprintf("Drop threshold set to %d g\n", g_drop_threshold_g);

    return 0;
}
MSH_CMD_EXPORT(set_drop, set alarm drop threshold in gram);
/*
 * 命令：buzzer_test
 *
 * 作用：
 *     测试蜂鸣器是否能响 1 秒。
 *
 * 使用：
 *     在串口终端输入：
 *     buzzer_test
 */
int buzzer_test(int argc, char **argv)
{
#if USE_BUZZER
    rt_kprintf("Buzzer test: ON\n");

    rt_pin_write(BUZZER_PIN, PIN_HIGH);

    rt_thread_mdelay(1000);

    rt_pin_write(BUZZER_PIN, PIN_LOW);

    rt_kprintf("Buzzer test: OFF\n");
#else
    rt_kprintf("Buzzer is disabled. Please set USE_BUZZER to 1.\n");
#endif

    return 0;
}

MSH_CMD_EXPORT(buzzer_test, test buzzer for 1 second);




#endif

/* ===================== 主函数 ===================== */

int main(void)
{
    hx711_init();

#if USE_BUZZER
    rt_pin_mode(BUZZER_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(BUZZER_PIN, PIN_LOW);
#endif


#if USE_CAMERA_TRIGGER
    rt_pin_mode(CAMERA_TRIGGER_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(CAMERA_TRIGGER_PIN, PIN_LOW);
#endif

    rt_kprintf("\n");
    rt_kprintf("=============================================\n");
    rt_kprintf("Food Anti-theft System Demo\n");
    rt_kprintf("Mode: HX711 Real Weight Mode\n");
    rt_kprintf("---------------------------------------------\n");
    rt_kprintf("Wiring:\n");
    rt_kprintf("  HX711 VCC  -> 3.3V\n");
    rt_kprintf("  HX711 GND  -> GND\n");
    rt_kprintf("  HX711 DOUT -> PA1\n");
    rt_kprintf("  HX711 SCK  -> PA0\n");
    rt_kprintf("---------------------------------------------\n");
    rt_kprintf("Commands:\n");
    rt_kprintf("  hx711_raw\n");
    rt_kprintf("  hx711_tare\n");
    rt_kprintf("  hx711_cal 500\n");
    rt_kprintf("  hx711_weight\n");
    rt_kprintf("  guard_start\n");
    rt_kprintf("  guard_status\n");
    rt_kprintf("  guard_stop\n");
    rt_kprintf("  alarm_off\n");
    rt_kprintf("  set_drop 300\n");
    rt_kprintf("=============================================\n");

    while (1)
    {
        guard_logic_update();
        rt_thread_mdelay(SAMPLE_PERIOD_MS);
    }

    return 0;
}

