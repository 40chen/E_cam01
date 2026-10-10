/*
 * web.c — 手机网页遥控
 *
 * **用 HTTP 轮询而不是 WebSocket**：摇杆每 100ms 发一个
 * `GET /drive?x=..&y=..`，板子这边就是一个无状态的 handler，不用管连接
 * 生命周期、不用处理异步发送失败。局域网里一次请求往返个位数毫秒，
 * 100ms 的间隔足够跟手，而代码量是 WebSocket 那条路的三分之一。
 * （参考项目用的是 WebSocket + Vue，那是给带 PSRAM 的 S3 主机用的；
 * C2 只有 272KB RAM、4MB flash，那套搬不过来也不划算。）
 *
 * **安全边界**：这个页面没有任何认证，谁连上同一个 WiFi 谁就能开这辆车。
 * 车就在用户脚边、局域网内，可以接受；但别把它接到公共网络上。
 * 页面里那句提醒是认真的。
 */
#include "web.h"
#include "esp_check.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "motion.h"
#include "wifi_prov.h"

static const char *TAG = "web";

/*---------------------------------------------------------------------------
 * 页面
 *
 * 一个几 KB 的内嵌单页：一个圆形摇杆（div + pointer 事件，不用 canvas——
 * 少一层坐标换算），松开/离开就归零。
 *
 * 轮询而不是"只在动的时候发"：**松开手要有一条明确的 0,0 发出去**，
 * 而定时轮询天然就带这个行为，还给看门狗持续喂食。停了不发的话，
 * 得靠对面 500ms 超时停车，手感就是"松手后还往前冲一下"。
 *-------------------------------------------------------------------------*/
static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>Tank</title><style>"
"html,body{margin:0;height:100%;background:#111;color:#eee;"
"font:16px -apple-system,system-ui,sans-serif;overflow:hidden;touch-action:none;"
"-webkit-user-select:none;user-select:none}"
"#wrap{display:flex;flex-direction:column;align-items:center;justify-content:center;"
"height:100%;gap:14px}"
"#pad{position:relative;width:min(78vw,320px);aspect-ratio:1;border-radius:50%;"
"background:#1e1e1e;border:2px solid #333}"
"#knob{position:absolute;left:50%;top:50%;width:38%;aspect-ratio:1;border-radius:50%;"
"background:#0a84ff;transform:translate(-50%,-50%);transition:background .15s}"
"#pad.on #knob{background:#30d158}"
"#st{color:#888;font-size:14px;text-align:center;line-height:1.5;padding:0 16px}"
".warn{color:#ff9f0a}</style></head><body><div id='wrap'>"
"<div id='pad'><div id='knob'></div></div>"
"<div id='st'>Drag to drive &middot; release to stop<br>"
"<span class='warn'>No authentication &mdash; anyone on this Wi-Fi can drive.</span>"
"</div></div><script>"
"var pad=document.getElementById('pad'),knob=document.getElementById('knob');"
"var x=0,y=0,touching=false;"
"function set(ev){"
" if(!touching)return;"
" var r=pad.getBoundingClientRect();"
" var t=ev.touches?ev.touches[0]:ev;"
" var dx=(t.clientX-r.left-r.width/2)/(r.width/2);"
" var dy=(r.height/2-(t.clientY-r.top))/(r.height/2);"
" var len=Math.hypot(dx,dy);"
" if(len>1){dx/=len;dy/=len;}else if(len<0.08){dx=0;dy=0;}"
" x=dx;y=dy;"
" knob.style.transform='translate(-50%,-50%) translate('+(dx*30)+'%,'+(-dy*30)+'%)';"
"}"
"function start(ev){ev.preventDefault();touching=true;pad.classList.add('on');set(ev);}"
"function end(){touching=false;x=0;y=0;pad.classList.remove('on');"
" knob.style.transform='translate(-50%,-50%)';}"
"pad.addEventListener('touchstart',start,{passive:false});"
"pad.addEventListener('touchmove',set,{passive:false});"
"pad.addEventListener('touchend',end);pad.addEventListener('touchcancel',end);"
"pad.addEventListener('mousedown',start);"
"document.addEventListener('mousemove',set);document.addEventListener('mouseup',end);"
"setInterval(function(){"
" fetch('/drive?x='+x.toFixed(2)+'&y='+y.toFixed(2)).catch(function(){});"
"},100);"
"</script></body></html>";

/*---------------------------------------------------------------------------
 * handlers
 *-------------------------------------------------------------------------*/
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* 页面没有外部资源，但加一条没坏处：手机上别让浏览器缓存住旧版本 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* 从 query 里取一个浮点参数，缺省值 def */
static float query_float(httpd_req_t *req, const char *key, float def)
{
    /* "x=-1.00&y=-1.00" 是 15 个字符，48 够宽裕；超了就当没传，
     * 也就是"不动"，安全方向 */
    char buf[48];
    if (httpd_req_get_url_query_len(req) == 0) {
        return def;
    }
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        return def;
    }
    char val[12];
    if (httpd_query_key_value(buf, key, val, sizeof(val)) != ESP_OK) {
        return def;
    }
    return strtof(val, NULL);
}

static esp_err_t drive_get(httpd_req_t *req)
{
    motion_drive(query_float(req, "x", 0.0f), query_float(req, "y", 0.0f));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

/* 给手机一个"这车活着吗"的探针，顺便报个 IP */
static esp_err_t status_get(httpd_req_t *req)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ip\":\"%s\",\"connected\":%s}",
             wifi_prov_ip(), wifi_prov_connected() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;        /* 手机来去频繁，别把连接表占满 */

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        { .uri = "/",       .method = HTTP_GET, .handler = page_get },
        { .uri = "/drive",  .method = HTTP_GET, .handler = drive_get },
        { .uri = "/status", .method = HTTP_GET, .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]),
                            TAG, "register %s", uris[i].uri);
    }

    ESP_LOGI(TAG, "遥控页面已就绪：http://%s/",
             wifi_prov_ip()[0] ? wifi_prov_ip() : "<等拿到 IP>");
    return ESP_OK;
}
