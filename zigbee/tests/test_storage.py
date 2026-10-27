"""Compile production storage.c against host file I/O and unused platform stubs.

Run with Python 3 and gcc on PATH. No device, SDK or third-party Python packages required.
Only the file persistence paths are exercised; this does not test LittleFS or the RTOS queue.
"""
from pathlib import Path
import subprocess
import tempfile


STUBS = r"""
#ifndef AIRLOOM_STORAGE_HOST_STUBS
#define AIRLOOM_STORAGE_HOST_STUBS
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct cJSON cJSON;
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_AddStringToObject(cJSON *, const char *, const char *);
cJSON *cJSON_AddNumberToObject(cJSON *, const char *, double);
cJSON *cJSON_AddNullToObject(cJSON *, const char *);
cJSON *cJSON_AddBoolToObject(cJSON *, const char *, bool);
cJSON *cJSON_AddObjectToObject(cJSON *, const char *);
bool cJSON_AddItemToArray(cJSON *, cJSON *);
void cJSON_Delete(cJSON *);
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef int esp_err_t;
typedef struct { size_t size; } esp_partition_t;
typedef struct {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
} esp_vfs_littlefs_conf_t;
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 255
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
const char *esp_err_to_name(esp_err_t);
uint32_t esp_random(void);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
int64_t esp_timer_get_time(void);
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
#if defined(_WIN32)
#define fsync _commit
#endif
#endif
"""

PLATFORM_IMPLEMENTATION = r"""
#include "platform.h"
/* These satisfy the unused RTOS/network entry points; tests call file logic only. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
const char *esp_err_to_name(esp_err_t value) { return "stub"; }
uint32_t esp_random(void) { return 1; }
uint32_t esp_get_free_heap_size(void) { return 100000; }
uint32_t esp_get_minimum_free_heap_size(void) { return 100000; }
int64_t esp_timer_get_time(void) { return 0; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label) { return NULL; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *data, size_t len) { return -1; }
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *c) { return -1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return NULL; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t delay) { return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { return 1; }
QueueHandle_t xQueueCreate(unsigned count, unsigned size) { return NULL; }
int xQueueReceive(QueueHandle_t q, void *item, uint32_t delay) { return 0; }
int xQueueSend(QueueHandle_t q, const void *item, uint32_t delay) { return 0; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) { return 0; }
void vTaskDelete(void *task) {}
cJSON *cJSON_CreateObject(void) { return NULL; }
cJSON *cJSON_CreateArray(void) { return NULL; }
cJSON *cJSON_AddStringToObject(cJSON *j, const char *name, const char *value) { return NULL; }
cJSON *cJSON_AddNumberToObject(cJSON *j, const char *name, double value) { return NULL; }
cJSON *cJSON_AddNullToObject(cJSON *j, const char *name) { return NULL; }
cJSON *cJSON_AddBoolToObject(cJSON *j, const char *name, bool value) { return NULL; }
cJSON *cJSON_AddObjectToObject(cJSON *j, const char *name) { return NULL; }
bool cJSON_AddItemToArray(cJSON *j, cJSON *item) { return false; }
void cJSON_Delete(cJSON *j) {}
#pragma GCC diagnostic pop
"""


def main():
    source = Path(__file__).resolve().with_name("test_storage.c")
    with tempfile.TemporaryDirectory(prefix="airloom-storage-test-") as temporary:
        root = Path(temporary)
        stubs = root / "stubs"
        stubs.mkdir()
        (stubs / "platform.h").write_text(STUBS, encoding="utf-8")
        for name in ["cJSON.h", "esp_littlefs.h", "esp_log.h", "esp_partition.h",
                     "esp_random.h", "esp_system.h", "esp_timer.h", "freertos/FreeRTOS.h",
                     "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"]:
            path = stubs / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "platform.h"\n', encoding="utf-8")
        executable = root / "test-storage.exe"
        implementation = root / "platform.c"
        implementation.write_text(PLATFORM_IMPLEMENTATION, encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(stubs), "-I", str(source.parent.parent / "main"),
                        str(source), str(implementation), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], cwd=root, check=True)


if __name__ == "__main__":
    main()
