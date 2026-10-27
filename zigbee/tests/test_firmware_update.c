#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "../main/firmware_update.c"

char test_password[80] = "test-password-12345";
static const esp_partition_t running = {.address=0x10000,.size=4096,.label="ota_0"};
static const esp_partition_t target = {.address=0x180000,.size=4096,.label="ota_1"};
static unsigned char image[512];
static unsigned begin_calls,abort_calls,end_calls,select_calls,notify_calls,restarts,deleted_tasks;
static size_t written_bytes;
static size_t body_offset_at_response;
static unsigned recv_failures;
static esp_err_t begin_error,write_error,end_error,select_error;
static bool task_failure,response_sent;
static void (*scheduled_restart)(void *);
static char observed_status_hash[65];

void test_log(const char *tag,const char *format,...) { (void)tag;(void)format; }
const char *esp_err_to_name(esp_err_t error) { (void)error;return "stub error"; }
static cJSON json;
cJSON *cJSON_CreateObject(void) { return &json; }
cJSON *cJSON_AddStringToObject(cJSON *j,const char *n,const char *v)
{
    if (!strcmp(n,"elf_sha256")) {
        assert(strlen(v)<sizeof observed_status_hash);
        memcpy(observed_status_hash,v,strlen(v)+1);
    }
    return j;
}
cJSON *cJSON_AddNumberToObject(cJSON *j,const char *n,double v) { (void)n;(void)v;return j; }
cJSON *cJSON_AddBoolToObject(cJSON *j,const char *n,bool v) { (void)n;(void)v;return j; }
void cJSON_Delete(cJSON *j) { (void)j; }
char *cJSON_PrintUnformatted(const cJSON *j) { (void)j;char *s=malloc(3);memcpy(s,"{}",3);return s; }
void cJSON_free(void *p) { free(p); }
static const char *header(httpd_req_t *r,const char *name)
{
    for (unsigned i=0;i<r->header_count;i++) if (!strcmp(r->headers[i].name,name)) return r->headers[i].value;
    return NULL;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name) { const char *s=header(r,name);return s?strlen(s):0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t size)
{
    const char *s=header(r,name);
    if (!s || strlen(s)>=size) return ESP_FAIL;
    memcpy(out,s,strlen(s)+1);return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *s) { snprintf(r->status,sizeof r->status,"%s",s);return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *s) { (void)r;(void)s;return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *a,const char *b) { (void)r;(void)a;(void)b;return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r,const char *s,int len)
{
    size_t n=len<0?strlen(s):(size_t)len;
    assert(n<sizeof r->response);memcpy(r->response,s,n);r->response[n]=0;
    body_offset_at_response=r->offset;response_sent=true;return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r,char *out,size_t wanted)
{
    if (r->offset>=r->break_at) { ++recv_failures;return -1; }
    size_t n=r->content_len-r->offset;
    if (n>wanted) n=wanted;
    if (n>31) n=31; /* Header and body may arrive across arbitrary TCP boundaries. */
    if (n>r->break_at-r->offset) n=r->break_at-r->offset;
    memcpy(out,r->body+r->offset,n);r->offset+=n;return (int)n;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t s,const httpd_uri_t *h) { (void)s;assert(h->handler);return ESP_OK; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &running; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { (void)p;return &target; }
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *h)
{ assert(p==&target && size<=target.size);++begin_calls;*h=7;return begin_error; }
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *p,size_t n)
{ assert(h==7 && p);if (!write_error) written_bytes+=n;return write_error; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { assert(h==7);++end_calls;return end_error; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { assert(h==7);++abort_calls;return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { assert(p==&target);++select_calls;return select_error; }
static const esp_app_desc_t app = {.version="test",.app_elf_sha256={
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}};
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
/* The SDK helper may return only the configured logging prefix, even with a
 * larger destination buffer. Status must encode app_desc's complete hash. */
int esp_app_get_elf_sha256(char *out,size_t size) { assert(size>=9);memcpy(out,"01234567",9);return 9; }
int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *a,unsigned p,TaskHandle_t *out)
{ (void)n;(void)s;(void)a;(void)p;if(task_failure)return 0;scheduled_restart=f;*out=(void*)1;return pdPASS; }
void vTaskDelete(TaskHandle_t t) { assert(t==(void*)1);++deleted_tasks; }
void vTaskDelay(TickType_t t) { assert(t==1000); }
unsigned ulTaskNotifyTake(int clear,TickType_t ticks) { (void)clear;(void)ticks;return 1; }
void xTaskNotifyGive(TaskHandle_t t) { assert(t==(void*)1 && response_sent);++notify_calls; }
void esp_restart(void) { ++restarts; }

static void reset(void)
{
    begin_calls=abort_calls=end_calls=select_calls=notify_calls=restarts=deleted_tasks=0;
    written_bytes=body_offset_at_response=0;recv_failures=0;
    begin_error=write_error=end_error=select_error=ESP_OK;
    task_failure=response_sent=false;scheduled_restart=NULL;
    atomic_store(&updating,false);
    strcpy(test_password,"test-password-12345");
    memset(image,0,sizeof image);
    esp_image_header_t h={.magic=ESP_IMAGE_HEADER_MAGIC,.chip_id=ESP_CHIP_ID_ESP32C6,.segment_count=1};
    esp_image_segment_header_t s={.data_len=sizeof image-sizeof h-sizeof s};
    esp_app_desc_t d={.magic_word=ESP_APP_DESC_MAGIC_WORD,.project_name="airloom_zigbee"};
    for (unsigned i=0;i<32;i++) d.app_elf_sha256[i]=(uint8_t)i;
    memcpy(image,&h,sizeof h);memcpy(image+sizeof h,&s,sizeof s);memcpy(image+sizeof h+sizeof s,&d,sizeof d);
}
static httpd_req_t request(void)
{
    return (httpd_req_t){.content_len=sizeof image,.body=image,.break_at=sizeof image,
        .headers={{PASSWORD_HEADER,"test-password-12345"},{"Content-Type","application/octet-stream"},
                  {"Origin","http://192.168.0.8"},{"Host","192.168.0.8"}},.header_count=4};
}
static void rejected_before_write(httpd_req_t *r,const char *status)
{
    assert(upload_handler(r)==ESP_FAIL);
    assert(!strncmp(r->status,status,3));
    assert(!begin_calls && !select_calls && !notify_calls && !restarts);
    assert(!atomic_load(&updating));
    bool no_drain=!strncmp(status,"413",3) || header(r,"Transfer-Encoding") ||
        r->content_len>MAX_REJECT_DRAIN_SIZE;
    size_t expected=no_drain?0:r->content_len<r->break_at?r->content_len:r->break_at;
    assert(r->offset==expected && body_offset_at_response==expected);
}

int main(void)
{
    _Static_assert(sizeof(esp_image_header_t)==24,"host image header");
    _Static_assert(sizeof(esp_app_desc_t)==256,"host app descriptor");
    reset();httpd_req_t r=request();r.headers[0].value="incorrect-password";rejected_before_write(&r,"401");
    reset();r=request();r.headers[0].value="";rejected_before_write(&r,"401");
    reset();r=request();r.headers[0].value="";r.content_len=MAX_REJECT_DRAIN_SIZE+1;rejected_before_write(&r,"401");
    reset();r=request();test_password[0]=0;rejected_before_write(&r,"503");
    reset();r=request();memset(test_password,'x',65);test_password[65]=0;rejected_before_write(&r,"503");
    reset();r=request();r.headers[2].value="http://evil.invalid";rejected_before_write(&r,"403");
    reset();r=request();r.content_len=PREFIX_SIZE-1;rejected_before_write(&r,"400");
    reset();r=request();r.content_len=target.size+1;rejected_before_write(&r,"413");
    reset();r=request();r.headers[1].value="multipart/form-data";rejected_before_write(&r,"415");
    reset();r=request();r.headers[4]=(test_header_t){"Transfer-Encoding","chunked"};r.header_count=5;rejected_before_write(&r,"400");
    reset();r=request();image[0]=0;rejected_before_write(&r,"400");
    reset();r=request();esp_image_header_t h;memcpy(&h,image,sizeof h);h.chip_id=7;memcpy(image,&h,sizeof h);rejected_before_write(&r,"400");
    reset();r=request();esp_app_desc_t d;memcpy(&d,image+32,sizeof d);strcpy(d.project_name,"wrong_project");memcpy(image+32,&d,sizeof d);rejected_before_write(&r,"400");
    reset();r=request();memcpy(&d,image+32,sizeof d);memset(d.project_name,'x',sizeof d.project_name);memcpy(image+32,&d,sizeof d);rejected_before_write(&r,"400");
    reset();r=request();r.break_at=60;rejected_before_write(&r,"400");assert(recv_failures==1);
    reset();r=request();r.headers[0].value="";r.break_at=60;rejected_before_write(&r,"401");assert(recv_failures==1);
    reset();r=request();atomic_store(&updating,true);assert(upload_handler(&r)==ESP_FAIL && !strncmp(r.status,"409",3) && !begin_calls && body_offset_at_response==sizeof image);

    reset();r=request();r.break_at=350;assert(upload_handler(&r)==ESP_FAIL);assert(begin_calls==1 && abort_calls==1 && !end_calls && !select_calls && !notify_calls && !atomic_load(&updating) && body_offset_at_response==350 && recv_failures==1);
    reset();r=request();write_error=ESP_FAIL;assert(upload_handler(&r)==ESP_FAIL);assert(abort_calls==1 && !select_calls && !notify_calls && body_offset_at_response==sizeof image);
    reset();r=request();begin_error=ESP_FAIL;assert(upload_handler(&r)==ESP_FAIL);assert(begin_calls==1 && !abort_calls && !select_calls && !notify_calls && body_offset_at_response==sizeof image);
    reset();r=request();end_error=ESP_ERR_OTA_VALIDATE_FAILED;assert(upload_handler(&r)==ESP_FAIL);assert(end_calls==1 && !abort_calls && !select_calls && !notify_calls && !atomic_load(&updating));
    reset();r=request();task_failure=true;assert(upload_handler(&r)==ESP_FAIL);assert(end_calls==1 && !select_calls && !notify_calls);
    reset();r=request();select_error=ESP_FAIL;assert(upload_handler(&r)==ESP_FAIL);assert(select_calls==1 && deleted_tasks==1 && !notify_calls && !atomic_load(&updating));

    reset();r=request();assert(upload_handler(&r)==ESP_OK);
    assert(begin_calls==1 && end_calls==1 && !abort_calls && select_calls==1 && notify_calls==1);
    assert(written_bytes==sizeof image && atomic_load(&updating) && !restarts);
    assert(strstr(r.response,"\"ok\":true") && strstr(r.response,"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
    assert(scheduled_restart);scheduled_restart(NULL);assert(restarts==1);
    assert(firmware_update_register((void*)1)==ESP_OK);
    observed_status_hash[0]=0;
    assert(firmware_update_status_json()!=NULL);
    assert(strlen(observed_status_hash)==64);
    assert(!strcmp(observed_status_hash,"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    puts("firmware update authentication, bounded body drain, origin, image bounds, interrupted transfer, validation, inactive-slot selection, and restart tests passed");
    return 0;
}
