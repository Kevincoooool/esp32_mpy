/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "errno.h"
#include <stdio.h>
#include <string.h>

#include "audio_element.h"
#include "audio_error.h"
#include "audio_mem.h"

#include "esp_log.h"
#include "vfs_stream.h"
#include "wav_head.h"

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"

#define FILE_WAV_SUFFIX_TYPE   "wav"
#define FILE_OPUS_SUFFIX_TYPE  "opus"
#define FILE_AMR_SUFFIX_TYPE   "amr"
#define FILE_AMRWB_SUFFIX_TYPE "Wamr"

static const char *TAG = "VFS_STREAM";

typedef enum {
    STREAM_TYPE_UNKNOW,
    STREAM_TYPE_WAV,
    STREAM_TYPE_OPUS,
    STREAM_TYPE_AMR,
    STREAM_TYPE_AMRWB,
} wr_stream_type_t;

typedef struct vfs_stream {
    audio_stream_type_t type;
    int block_size;
    bool is_open;
    FIL file;
    wr_stream_type_t w_type;
} vfs_stream_t;

static wr_stream_type_t get_type(const char *str)
{
    char *relt = strrchr(str, '.');
    if (relt != NULL) {
        relt++;
        ESP_LOGD(TAG, "result = %s", relt);
        if (strncasecmp(relt, FILE_WAV_SUFFIX_TYPE, 3) == 0) {
            return STREAM_TYPE_WAV;
        } else if (strncasecmp(relt, FILE_OPUS_SUFFIX_TYPE, 4) == 0) {
            return STREAM_TYPE_OPUS;
        } else if (strncasecmp(relt, FILE_AMR_SUFFIX_TYPE, 3) == 0) {
            return STREAM_TYPE_AMR;
        } else if (strncasecmp(relt, FILE_AMRWB_SUFFIX_TYPE, 4) == 0) {
            return STREAM_TYPE_AMRWB;
        } else {
            return STREAM_TYPE_UNKNOW;
        }
    } else {
        return STREAM_TYPE_UNKNOW;
    }
}

static int get_len(FIL *stream)
{
    return f_size(stream);
}

static esp_err_t _vfs_open(audio_element_handle_t self)
{
    vfs_stream_t *vfs = (vfs_stream_t *)audio_element_getdata(self);

    audio_element_info_t info;
    char *uri = audio_element_get_uri(self);
    if (uri == NULL) {
        ESP_LOGE(TAG, "Error, uri is not set");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "_vfs_open, uri:%s", uri);
//    char *path = strstr(uri, "/sdcard");
    char *path = strstr(uri, "/")+1;
    audio_element_getinfo(self, &info);
    if (path == NULL) {
        ESP_LOGE(TAG, "Error, need file path to open");
        return ESP_FAIL;
    }
    if (vfs->is_open) {
        ESP_LOGE(TAG, "already opened");
        return ESP_FAIL;
    }
    
    int err = 0;
    const char *path_out = NULL;
    mp_vfs_mount_t *vfs_mount = mp_vfs_lookup_path(path, &path_out);

    if (vfs->type == AUDIO_STREAM_READER) {
        err = f_open(&(((fs_user_mount_t *)vfs_mount->obj)->fatfs), &vfs->file, path_out, FA_READ);
        info.total_bytes = get_len(&vfs->file);
        
        printf("test read path = %s,path_out =%s\n",path,path_out);
        ESP_LOGI(TAG, "File size is %d byte,pos:%d", (int)info.total_bytes, (int)info.byte_pos);
        if (err == 0 && (info.byte_pos > 0)) {
            if (f_lseek(&vfs->file, info.byte_pos) != 0) {
                ESP_LOGE(TAG, "Error seek file");
                return ESP_FAIL;
            }
        }
    } else if (vfs->type == AUDIO_STREAM_WRITER) {
        err = f_open(&(((fs_user_mount_t *)vfs_mount->obj)->fatfs), &vfs->file, path_out, FA_WRITE | FA_CREATE_ALWAYS);
        vfs->w_type = get_type(path);
        unsigned int bw = 0;
        printf("test write\n");
        printf("test write path = %s,path_out =%s\n",path,path_out);
        if (err == 0 && STREAM_TYPE_WAV == vfs->w_type) {
            wav_header_t info = { 0 };
            f_write(&vfs->file, &info, sizeof(wav_header_t), &bw);
            f_sync(&vfs->file);
        } else if (err == 0 && (STREAM_TYPE_AMR == vfs->w_type)) {
            f_write(&vfs->file, "#!AMR\n", 6, &bw);
            f_sync(&vfs->file);
        } else if (err == 0 && (STREAM_TYPE_AMRWB == vfs->w_type)) {
            f_write(&vfs->file, "#!AMR-WB\n", 9, &bw);
            f_sync(&vfs->file);
        }
    } else {
        ESP_LOGE(TAG, "vfs must be Reader or Writer");
        return ESP_FAIL;
    }
    if (err != 0) {

        ESP_LOGE(TAG, "Failed to open file %s, %d", path, err);
        return ESP_FAIL;
    }
    vfs->is_open = true;
    if (info.byte_pos && f_lseek(&vfs->file, info.byte_pos) != 0) {
        ESP_LOGE(TAG, "Failed to seek to %d/%d", (int)info.byte_pos, (int)info.total_bytes);
        return ESP_FAIL;
    }

    return audio_element_setinfo(self, &info);
}

static int _vfs_read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    vfs_stream_t *vfs = (vfs_stream_t *)audio_element_getdata(self);
    audio_element_info_t info;
    audio_element_getinfo(self, &info);

    ESP_LOGD(TAG, "read len=%d, pos=%d/%d", len, (int)info.byte_pos, (int)info.total_bytes);
    unsigned int rlen = 0;
    f_read(&vfs->file, buffer, len, &rlen);
    if (rlen <= 0) {
        ESP_LOGW(TAG, "No more data,ret:%d", rlen);
        rlen = 0;
    } else {
        info.byte_pos += rlen;
        audio_element_setinfo(self, &info);
    }
    return rlen;
}

static int _vfs_write(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    vfs_stream_t *vfs = (vfs_stream_t *)audio_element_getdata(self);
    audio_element_info_t info;
    audio_element_getinfo(self, &info);
    unsigned int wlen = 0;
    f_write(&vfs->file, buffer, len, &wlen);
    f_sync(&vfs->file);
    ESP_LOGD(TAG, "f_write,%d, errno:%d,pos:%d", wlen, errno, (int)info.byte_pos);
    if (wlen > 0) {
        info.byte_pos += wlen;
        audio_element_setinfo(self, &info);
    }
    return wlen;
}

static int _vfs_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    int w_size = 0;
    if (r_size > 0) {
        w_size = audio_element_output(self, in_buffer, r_size);
    } else {
        w_size = r_size;
    }
    return w_size;
}

static esp_err_t _vfs_close(audio_element_handle_t self)
{
    vfs_stream_t *vfs = (vfs_stream_t *)audio_element_getdata(self);

    if (AUDIO_STREAM_WRITER == vfs->type
        && STREAM_TYPE_WAV == vfs->w_type) {
        wav_header_t *wav_info = (wav_header_t *)audio_malloc(sizeof(wav_header_t));

        AUDIO_MEM_CHECK(TAG, wav_info, return ESP_ERR_NO_MEM);

        if (f_lseek(&vfs->file, 0) != 0) {
            ESP_LOGE(TAG, "Error seek file ,line=%d", __LINE__);
        }
        audio_element_info_t info;
        audio_element_getinfo(self, &info);
        wav_head_init(wav_info, info.sample_rates, info.bits, info.channels);
        wav_head_size(wav_info, (uint32_t)info.byte_pos);
        unsigned int wl = 0;
        f_write(&vfs->file, wav_info, sizeof(wav_header_t), &wl);
        f_sync(&vfs->file);
        audio_free(wav_info);
    }

    if (vfs->is_open) {
        f_close(&vfs->file);
        vfs->is_open = false;
    }
    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        audio_element_report_info(self);
        audio_element_info_t info = { 0 };
        audio_element_getinfo(self, &info);
        info.byte_pos = 0;
        audio_element_setinfo(self, &info);
    }
    return ESP_OK;
}

static esp_err_t _vfs_destroy(audio_element_handle_t self)
{
    vfs_stream_t *vfs = (vfs_stream_t *)audio_element_getdata(self);
    audio_free(vfs);
    return ESP_OK;
}

audio_element_handle_t vfs_stream_init(vfs_stream_cfg_t *config)
{
    audio_element_handle_t el;
    vfs_stream_t *vfs = audio_calloc(1, sizeof(vfs_stream_t));

    AUDIO_MEM_CHECK(TAG, vfs, return NULL);

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = _vfs_open;
    cfg.close = _vfs_close;
    cfg.process = _vfs_process;
    cfg.destroy = _vfs_destroy;
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.out_rb_size = config->out_rb_size;
    cfg.buffer_len = config->buf_sz;
    if (cfg.buffer_len == 0) {
        cfg.buffer_len = VFS_STREAM_BUF_SIZE;
    }

    cfg.tag = "file";
    vfs->type = config->type;

    if (config->type == AUDIO_STREAM_WRITER) {
        cfg.write = _vfs_write;
    } else {
        cfg.read = _vfs_read;
    }
    el = audio_element_init(&cfg);

    AUDIO_MEM_CHECK(TAG, el, goto _vfs_init_exit);
    audio_element_setdata(el, vfs);
    return el;
_vfs_init_exit:
    audio_free(vfs);
    return NULL;
}
