"""Run the production OTA HTTP handlers against host-side SDK stubs.

Requires Python 3 and gcc on PATH. Does not access a board or validate SDK flash I/O.
"""
from pathlib import Path
import subprocess
import tempfile


STUBS = r"""
#ifndef AIRLOOM_OTA_TEST_PLATFORM
#define AIRLOOM_OTA_TEST_PLATFORM
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_OTA_VALIDATE_FAILED 2
const char *esp_err_to_name(esp_err_t error);
typedef struct cJSON { int unused; } cJSON;
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddStringToObject(cJSON *,const char *,const char *);
cJSON *cJSON_AddNumberToObject(cJSON *,const char *,double);
cJSON *cJSON_AddBoolToObject(cJSON *,const char *,bool);
void cJSON_Delete(cJSON *);
char *cJSON_PrintUnformatted(const cJSON *);
void cJSON_free(void *);
typedef struct { const char *name; const char *value; } test_header_t;
typedef struct {
    size_t content_len;
    const unsigned char *body;
    size_t offset, break_at;
    test_header_t headers[8];
    unsigned header_count;
    char status[64], response[256];
} httpd_req_t;
typedef void *httpd_handle_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define HTTP_GET 1
#define HTTP_POST 2
#define HTTPD_RESP_USE_STRLEN -1
size_t httpd_req_get_hdr_value_len(httpd_req_t *,const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *,const char *,char *,size_t);
esp_err_t httpd_resp_set_status(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
esp_err_t httpd_resp_send(httpd_req_t *,const char *,int);
int httpd_req_recv(httpd_req_t *,char *,size_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t *);
typedef struct { uint32_t address; size_t size; char label[16]; } esp_partition_t;
typedef uint32_t esp_ota_handle_t;
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_begin(const esp_partition_t *,size_t,esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t,const void *,size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_IMAGE_MAX_SEGMENTS 16
#define ESP_CHIP_ID_ESP32C6 13
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
typedef struct __attribute__((packed)) {
    uint8_t magic,segment_count,spi_mode,spi_speed_size;
    uint32_t entry;
    uint8_t wp,drive[3];
    uint16_t chip_id;
    uint8_t min_rev;
    uint16_t min_rev_full,max_rev_full;
    uint8_t reserved[4],hash_appended;
} esp_image_header_t;
typedef struct { uint32_t load_addr,data_len; } esp_image_segment_header_t;
typedef struct {
    uint32_t magic_word,secure_version,reserved1[2];
    char version[32],project_name[32],time[16],date[16],idf_ver[32];
    uint8_t app_elf_sha256[32],reserved2[80];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int esp_app_get_elf_sha256(char *,size_t);
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
unsigned ulTaskNotifyTake(int,TickType_t);
void xTaskNotifyGive(TaskHandle_t);
void esp_restart(void);
void test_log(const char *,const char *,...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
extern char test_password[80];
#define CONFIG_AIRLOOM_UPDATE_PASSWORD test_password
#endif
"""


def main():
    source = Path(__file__).resolve().with_name("test_firmware_update.c")
    with tempfile.TemporaryDirectory(prefix="airloom-ota-test-") as temporary:
        root = Path(temporary)
        stubs = root / "stubs"
        stubs.mkdir()
        (stubs / "platform.h").write_text(STUBS, encoding="utf-8")
        for name in ["cJSON.h", "esp_err.h", "esp_http_server.h", "esp_app_desc.h",
                     "esp_app_format.h", "esp_log.h", "esp_ota_ops.h", "esp_system.h",
                     "sdkconfig.h", "freertos/FreeRTOS.h", "freertos/task.h"]:
            target = stubs / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "platform.h"\n', encoding="utf-8")
        executable = root / "test-firmware-update.exe"
        subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(stubs), "-I", str(source.parent.parent / "main"),
                        str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
