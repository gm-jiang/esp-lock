#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "esp_codec_dev.h"
#include "codec_dev_gpio.h"
#include "codec_dev_defaults.h"
#include "audio_board.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "es8156.h"
#include "es7243e.h"
#include "es7243.h"
#include "es7210.h"
#include "es8311.h"
#include "zl38063.h"
#include "es8388.h"
#include "es8374.h"
#include "tas5805m.h"

#define TAG "Render"

#define LOG_ON_FAIL(ret)                                                \
    if (ret != 0) {                                                     \
        ESP_LOGE(TAG, "Fail at %s:%d ret %d", __func__, __LINE__, ret); \
    }
#define BREAK_ON_FAIL(ret)                                              \
    if (ret != 0) {                                                     \
        ESP_LOGE(TAG, "Fail at %s:%d ret %d", __func__, __LINE__, ret); \
        break;                                                          \
    }

//#define BOARD_NAME "ESP32_KORVO_DU1906"
// #define BOARD_NAME         "ESP32_S3_KORVO2_V3"
//#define BOARD_NAME "ESP_LYRATD_MSC_V2_1"
//#define BOARD_NAME "ESP_LYRAT_V4_2"
//#define BOARD_NAME "ESP_LYRAT_MINI_V1_1"
#define BOARD_NAME "ESP32_S3_BOX_LITE"
//#define BOARD_NAME "ESP32_S3_BOX"
#define MAX_RENDER_DEV_NUM (2)

typedef struct {
    audio_board_media_t          ctrl_media[MAX_RENDER_DEV_NUM];
    uint8_t                      ctrl_port[MAX_RENDER_DEV_NUM];
    const audio_codec_ctrl_if_t *ctrl_if[MAX_RENDER_DEV_NUM];
    audio_board_media_t          data_media[MAX_RENDER_DEV_NUM];
    uint8_t                      data_port[MAX_RENDER_DEV_NUM];
    const audio_codec_data_if_t *data_if[MAX_RENDER_DEV_NUM];
    const audio_codec_if_t      *codec_if[MAX_RENDER_DEV_NUM];
    const audio_codec_gpio_if_t *gpio_if;
    esp_codec_dev_handle_t       play_handle;
    esp_codec_dev_handle_t       rec_handle;
    int                          play_vol;
    int16_t                      max_value;
} render_res_t;

typedef union {
    es8156_codec_cfg_t   es8156_cfg;
    es7210_codec_cfg_t   es7210_cfg;
    es8311_codec_cfg_t   es8311_cfg;
    es7243e_codec_cfg_t  es7243e_cfg;
    es7243_codec_cfg_t   es7243_cfg;
    zl38063_codec_cfg_t  zl38063_cfg;
    es8388_codec_cfg_t   es8388_cfg;
    es8374_codec_cfg_t   es8374_cfg;
    tas5805m_codec_cfg_t tas5805m_cfg;
} render_codec_cfg_t;

typedef enum {
    KEY_ID_VOL_UP,
    KEY_ID_VOL_DOWN,
    KEY_ID_VOL_MAX,
} key_id_t;

static render_res_t render_res;
static audio_board_cfg_t *board_cfg;

extern const uint8_t pcm_start[] asm("_binary_file_2_8000_pcm_start");
extern const uint8_t pcm_end[] asm("_binary_file_2_8000_pcm_end");

const uint8_t* check_wav_size(codec_sample_info_t* fs, int* size)
{
    const uint8_t* s = pcm_start;
    const uint8_t* e = pcm_end;
    if (memcmp(s, "RIFF", 4) == 0 &&
        memcmp(s + 8, "WAVE", 4) == 0) {
        s += 12;
        while (s < e) {
            unsigned int chunk_size = *(unsigned int*) (s + 4);
            if (memcmp(s, "fmt ", 4) == 0) {
                fs->channel = (uint8_t)*(short*)(s+10);
                fs->sample_rate = (uint32_t)*(int*)(s+12);
                fs->bits_per_sample = (uint8_t)*(short*)(s+22);
            } else if (memcmp(s, "data", 4) == 0){
                s += 8;
                int left = e - s;
                if (left > chunk_size) {
                    left = chunk_size;
                }
                *size = left;
                ESP_LOGI(TAG, "sample:%d channel:%d bits:%d size %d",
                    fs->sample_rate, fs->channel, fs->bits_per_sample, left);
                return s;
            }
            s += chunk_size + 8;
        }
    }
    return pcm_start;
}

static int play_internal()
{
    if (render_res.play_handle == NULL) {
        ESP_LOGE(TAG, "Playback device not found");
        return 0;
    }
    codec_sample_info_t fs = {
        .bits_per_sample = 16,
        .sample_rate = 8000,
        .channel = 2,
    };
    int pcm_size = pcm_end - pcm_start;
    const uint8_t *pcm_pos = check_wav_size(&fs, &pcm_size);
    const uint8_t *pcm_limit = pcm_pos + pcm_size;
    int ret = esp_codec_dev_open(render_res.play_handle, &fs);
    int size = 1024;
    if (ret == 0) {
        esp_codec_dev_set_out_vol(render_res.play_handle, render_res.play_vol);
        int limit_size = 20 * fs.sample_rate * (fs.bits_per_sample >> 3) * fs.channel;
        int len = 0;
        while (len < limit_size) {
            ret = size;
            if (pcm_pos + size > pcm_limit) {
                ret = pcm_limit - pcm_pos;
            }
            if (ret > 0) {
                int res = esp_codec_dev_write(render_res.play_handle, (void *) pcm_pos, ret);
                BREAK_ON_FAIL(res);
                len += ret;
                pcm_pos += ret;
            }
            if (ret != 1024) {
                break;
            }
        }
    } else {
        ESP_LOGE(TAG, "Fail to do play test");
    }
    if (render_res.play_handle) {
        esp_codec_dev_close(render_res.play_handle);
    }
    ESP_LOGI(TAG, "play internal finished");
    return 0;
}

static void add_ctrl_if(audio_board_media_t media, uint8_t port, const audio_codec_ctrl_if_t *ctrl_if)
{
    int i;
    for (i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.ctrl_if[i] == NULL) {
            render_res.ctrl_if[i] = ctrl_if;
            render_res.ctrl_media[i] = media;
            render_res.ctrl_port[i] = port;
            break;
        }
    }
    if (i >= MAX_RENDER_DEV_NUM) {
        ESP_LOGE(TAG, "too many ctrl_if try enlarge MAX_RENDER_DEV_NUM(%d)", MAX_RENDER_DEV_NUM);
    }
}

static const audio_codec_ctrl_if_t *get_ctrl_if(audio_board_media_t media, uint8_t port)
{
    for (int i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.ctrl_if[i] && render_res.ctrl_media[i] == media && render_res.ctrl_port[i] == port) {
            return render_res.ctrl_if[i];
        }
    }
    return NULL;
}

static void add_data_if(audio_board_media_t media, uint8_t port, const audio_codec_data_if_t *data_if)
{
    int i;
    for (i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.data_if[i] == NULL) {
            render_res.data_if[i] = data_if;
            render_res.data_media[i] = media;
            render_res.data_port[i] = port;
            break;
        }
    }
    if (i >= MAX_RENDER_DEV_NUM) {
        ESP_LOGE(TAG, "too many data_if try enlarge MAX_RENDER_DEV_NUM(%d)", MAX_RENDER_DEV_NUM);
    }
}

static const audio_codec_data_if_t *get_data_if(audio_board_media_t media, uint8_t port)
{
    for (int i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.data_if[i] && render_res.data_media[i] == media && render_res.data_port[i] == port) {
            return render_res.data_if[i];
        }
    }
    return NULL;
}

static void add_codec_if(const audio_codec_if_t *codec_if)
{
    int i;
    for (i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.codec_if[i] == NULL) {
            render_res.codec_if[i] = codec_if;
            break;
        }
    }
    if (i >= MAX_RENDER_DEV_NUM) {
        ESP_LOGE(TAG, "too many codec num try enlarge MAX_RENDER_DEV_NUM(%d)", MAX_RENDER_DEV_NUM);
    }
}

static uint8_t get_codec_default_addr(audio_board_codec_type_t codec_type)
{
    switch (codec_type) {
        case AUDIO_BOARD_CODEC_ES8311:
            return ES8311_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_ES7210:
            return ES7210_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_ES8388:
            return ES8388_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_ES7243:
            return ES7243_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_ES7243E:
            return ES7243E_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_ES8374:
            return ES8374_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_ES8156:
            return ES8156_CODEC_DEFAULT_ADDR;
        case AUDIO_BOARD_CODEC_TAS5805M:
            return TAS5805M_CODEC_DEFAULT_ADDR;
        default:
            return 0;
    }
}

static void get_codec_cfg(audio_board_codec_type_t codec_type, const audio_codec_ctrl_if_t *ctrl_if,
                          audio_board_codec_io_cfg_t *io_cfg, render_codec_cfg_t *codec_cfg)
{
    switch (codec_type) {
        case AUDIO_BOARD_CODEC_ES8311: {
            es8311_codec_cfg_t *es8311_cfg = &codec_cfg->es8311_cfg;
            es8311_cfg->codec_mode = io_cfg->codec_mode;
            es8311_cfg->ctrl_if = ctrl_if;
            es8311_cfg->pa_pin = io_cfg->pa_pin;
            es8311_cfg->use_mclk = io_cfg->use_mclk;
        } break;
        case AUDIO_BOARD_CODEC_ES7210: {
            es7210_codec_cfg_t *es7210_cfg = &codec_cfg->es7210_cfg;
            es7210_cfg->ctrl_if = ctrl_if;
            es7210_cfg->master_mode = io_cfg->master_mode;
            es7210_cfg->mic_selected = 13; // io_cfg->mic_channel_sel;
        } break;
        case AUDIO_BOARD_CODEC_ES8388: {
            es8388_codec_cfg_t *es8388_cfg = &codec_cfg->es8388_cfg;
            es8388_cfg->ctrl_if = ctrl_if;
            es8388_cfg->codec_mode = io_cfg->codec_mode;
            es8388_cfg->pa_pin = io_cfg->pa_pin;
            es8388_cfg->master_mode = io_cfg->master_mode;
        } break;
        case AUDIO_BOARD_CODEC_ES8374: {
            es8374_codec_cfg_t *es8374_cfg = &codec_cfg->es8374_cfg;
            es8374_cfg->ctrl_if = ctrl_if;
            es8374_cfg->codec_mode = io_cfg->codec_mode;
            es8374_cfg->master_mode = io_cfg->master_mode;
            es8374_cfg->pa_pin = io_cfg->pa_pin;
        } break;
        case AUDIO_BOARD_CODEC_ES8156: {
            es8156_codec_cfg_t *es8156_cfg = &codec_cfg->es8156_cfg;
            es8156_cfg->ctrl_if = ctrl_if;
            es8156_cfg->pa_pin = io_cfg->pa_pin;
        } break;
        case AUDIO_BOARD_CODEC_ES7243E: {
            es7243e_codec_cfg_t *es7243e_cfg = &codec_cfg->es7243e_cfg;
            es7243e_cfg->ctrl_if = ctrl_if;
        } break;
        case AUDIO_BOARD_CODEC_ES7243: {
            es7243_codec_cfg_t *es7243_cfg = &codec_cfg->es7243_cfg;
            es7243_cfg->ctrl_if = ctrl_if;
        } break;
        case AUDIO_BOARD_CODEC_TAS5805M: {
            tas5805m_codec_cfg_t *tas5805m_cfg = &codec_cfg->tas5805m_cfg;
            tas5805m_cfg->ctrl_if = ctrl_if;
            tas5805m_cfg->codec_mode = io_cfg->codec_mode;
            tas5805m_cfg->master_mode = io_cfg->master_mode;
            tas5805m_cfg->reset_pin = io_cfg->reset_pin;
        } break;
        case AUDIO_BOARD_CODEC_ZL38063: {
            zl38063_codec_cfg_t *zl38063_cfg = &codec_cfg->zl38063_cfg;
            zl38063_cfg->ctrl_if = ctrl_if;
            zl38063_cfg->codec_mode = io_cfg->codec_mode;
            zl38063_cfg->pa_pin = io_cfg->pa_pin;
            zl38063_cfg->reset_pin = io_cfg->reset_pin;
        } break;
        default:
            break;
    }
}
static int init_render()
{
    render_res.gpio_if = audio_codec_new_gpio_if();
    audio_codec_set_gpio_if(render_res.gpio_if);
    int ret = 0;
    for (int i = 0; i < board_cfg->codec_num; i++) {
        audio_board_codec_cfg_t *codec_cfg = &board_cfg->codec_cfg[i];
        ESP_LOGI(TAG, "init codec %d start\n", codec_cfg->codec_type);
        const audio_codec_ctrl_if_t *ctrl_if = get_ctrl_if(codec_cfg->ctrl_media, codec_cfg->ctrl_port);
        if (ctrl_if == NULL) {
            switch (codec_cfg->ctrl_media) {
                case AUDIO_BOARD_MEDIA_I2C:
                    if (codec_cfg->ctrl_port < board_cfg->i2c_dev_num) {
                        codec_i2c_dev_cfg_t *dev_cfg = &board_cfg->i2c_dev_cfg[codec_cfg->ctrl_port];
                        if (dev_cfg->addr == 0) {
                            dev_cfg->addr = get_codec_default_addr(codec_cfg->codec_type);
                        }
                        ctrl_if = audio_codec_new_i2c_ctrl_if(dev_cfg);
                        ESP_LOGI(TAG, "Use i2c port %d addr:%02x\n", dev_cfg->port, dev_cfg->addr);
                    }
                    break;
                case AUDIO_BOARD_MEDIA_SPI:
                    if (codec_cfg->ctrl_port < board_cfg->spi_dev_num) {
                        codec_spi_dev_cfg_t *dev_cfg = &board_cfg->spi_dev_cfg[codec_cfg->ctrl_port];
                        ctrl_if = audio_codec_new_spi_ctrl_if(dev_cfg);
                    }
                    break;
                default:
                    break;
            }
            if (ctrl_if) {
                add_ctrl_if(codec_cfg->ctrl_media, codec_cfg->ctrl_port, ctrl_if);
                ESP_LOGI(TAG, "Add to ctrl if OK\n");
            }
        }
        const audio_codec_data_if_t *data_if = get_data_if(codec_cfg->data_media, codec_cfg->data_port);
        if (data_if == NULL) {
            switch (codec_cfg->data_media) {
                case AUDIO_BOARD_MEDIA_I2S:
                    if (codec_cfg->data_port < board_cfg->i2s_dev_num) {
                        codec_i2s_dev_cfg_t *dev_cfg = &board_cfg->i2s_dev_cfg[codec_cfg->data_port];
                        data_if = audio_codec_new_i2s_data_if(dev_cfg);
                        ESP_LOGI(TAG, "Use i2s port %d\n", dev_cfg->port);
                    }
                    break;
                default:
                    break;
            }
            if (data_if) {
                ESP_LOGI(TAG, "Add to data if OK\n");
                add_data_if(codec_cfg->data_media, codec_cfg->data_port, data_if);
            }
        }
        if (data_if == NULL) {
            ESP_LOGE(TAG, "Data interface must set for codec %d", i);
            continue;
        }
        const audio_codec_if_t *codec_if = NULL;
        audio_board_codec_io_cfg_t *io_cfg = &codec_cfg->io_cfg;
        render_codec_cfg_t codec_io_cfg = {0};
        get_codec_cfg(codec_cfg->codec_type, ctrl_if, io_cfg, &codec_io_cfg);
        ESP_LOGI(TAG, "Set ctrl if %p\n", ctrl_if);
        switch (codec_cfg->codec_type) {
            case AUDIO_BOARD_CODEC_ES8311:
                codec_if = es8311_codec_new(&codec_io_cfg.es8311_cfg);
                break;
            case AUDIO_BOARD_CODEC_ES7210:
                codec_if = es7210_codec_new(&codec_io_cfg.es7210_cfg);
                break;
            case AUDIO_BOARD_CODEC_ES8388:
                codec_if = es8388_codec_new(&codec_io_cfg.es8388_cfg);
                break;
            case AUDIO_BOARD_CODEC_ES8374:
                codec_if = es8374_codec_new(&codec_io_cfg.es8374_cfg);
                break;
            case AUDIO_BOARD_CODEC_ES8156:
                codec_if = es8156_codec_new(&codec_io_cfg.es8156_cfg);
                break;
            case AUDIO_BOARD_CODEC_ES7243:
                codec_if = es7243_codec_new(&codec_io_cfg.es7243_cfg);
                break;
            case AUDIO_BOARD_CODEC_ES7243E:
                codec_if = es7243e_codec_new(&codec_io_cfg.es7243e_cfg);
                break;
            case AUDIO_BOARD_CODEC_TAS5805M:
                codec_if = tas5805m_codec_new(&codec_io_cfg.tas5805m_cfg);
                break;
            case AUDIO_BOARD_CODEC_ZL38063:
                codec_if = zl38063_codec_new(&codec_io_cfg.zl38063_cfg);
                break;
            default:
                break;
        }
        if (codec_if) {
            add_codec_if(codec_if);
            ESP_LOGI(TAG, "Add codec %d OK\n", codec_cfg->codec_type);
        }
        esp_codec_dev_cfg_t dev_cfg = {
            .codec_if = codec_if,
            .data_if = data_if,
        };
        // TODO we only consider one playback and one record case
        if (io_cfg->codec_mode & CODEC_WORK_MODE_ADC) {
            if (render_res.rec_handle) {
                ESP_LOGI(TAG, "Only support one record now");
                continue;
            }
            dev_cfg.dev_type |= CODEC_DEV_TYPE_IN;
        }
        if (io_cfg->codec_mode & CODEC_WORK_MODE_DAC) {
            if (render_res.play_handle) {
                ESP_LOGI(TAG, "Only support one playback now");
                continue;
            }
            dev_cfg.dev_type |= CODEC_DEV_TYPE_OUT;
        }
        esp_codec_dev_handle_t dev_handle = esp_codec_dev_new(&dev_cfg);
        ESP_LOGI(TAG, "%d device type %p\n", dev_cfg.dev_type, dev_handle);
        if (dev_cfg.dev_type & CODEC_DEV_TYPE_IN) {
            render_res.rec_handle = dev_handle;
        }
        if (dev_cfg.dev_type & CODEC_DEV_TYPE_OUT) {
            render_res.play_handle = dev_handle;
            esp_codec_dev_hw_gain_t hw_gain = {
                .codec_dac_voltage = 3.3,
                .pa_voltage = 5.0,
                .pa_gain = codec_cfg->pa_gain,
            };
            esp_codec_dev_set_hw_gain(dev_handle, &hw_gain);
        }
    }
    return ret;
}

static void deinit_render()
{
    uint8_t i;
    // Delete codec device
    if (render_res.play_handle) {
        esp_codec_dev_delete(render_res.play_handle);
    }
    if (render_res.rec_handle && render_res.rec_handle != render_res.play_handle) {
        esp_codec_dev_delete(render_res.rec_handle);
    }
    render_res.play_handle = render_res.rec_handle = NULL;
    // Delete codecs interface
    for (i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.codec_if[i]) {
            audio_codec_delete_codec_if(render_res.codec_if[i]);
            render_res.codec_if[i] = NULL;
        }
    }
    // Delete Data interface
    for (i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.data_if[i]) {
            audio_codec_delete_data_if(render_res.data_if[i]);
            render_res.data_if[i] = NULL;
        }
    }
    // delete Control interface
    for (i = 0; i < MAX_RENDER_DEV_NUM; i++) {
        if (render_res.ctrl_if[i]) {
            audio_codec_delete_ctrl_if(render_res.ctrl_if[i]);
            render_res.ctrl_if[i] = NULL;
        }
    }
    // Unregister GPIO interface
    audio_codec_set_gpio_if(NULL);
    audio_codec_delete_gpio_if(render_res.gpio_if);
    render_res.gpio_if = NULL;
}

void play_open_door(void) 
{
    int ret;
    board_cfg = audio_board_get_cfg(BOARD_NAME);
    if (board_cfg == NULL) {
        ESP_LOGE(TAG, "Fail to get board for %s", BOARD_NAME);
        return;
    }
    ret = audio_board_install_device(board_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to install driver");
        return;
    }
    ret = init_render();
    LOG_ON_FAIL(ret);
    codec_dev_vol_map_t vol_map[2] = {
        {.db_value = -96, .vol = 0},
        {.db_value = 9, .vol = 100},
    };
    esp_codec_dev_vol_curve_t vol_curve = {
        .vol_map = vol_map,
        .count = 2,
    };
    esp_codec_dev_set_vol_curve(render_res.play_handle, &vol_curve);
    //Set volume level
    render_res.play_vol = 90;
    for (int i = 0; i < 1; i++) {
        play_internal();
    }

    if (render_res.rec_handle) {
        esp_codec_dev_close(render_res.rec_handle);
    }
    if (render_res.play_handle) {
        esp_codec_dev_close(render_res.play_handle);
    }
    deinit_render();
    audio_board_uninstall_device(board_cfg);
    audio_board_free_cfg(board_cfg);
}

#if 0
void app_main()
{
    configure_lock();
    lock_control(0);

    for (int i = 0; i < 3; i++) {
        lock_control(1);
        esp_rom_delay_us(1000*1000);
        lock_control(0);
    }
    play_open_door();
    return;
}
#endif
