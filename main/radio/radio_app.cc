#include "radio_app.h"
#ifdef RADIO_RUN_SELF_TESTS
#include "radio_self_test.h"
#endif
#include "hls_playlist.h"
#include "stations.h"
#include "http_client.h"
#include <cstring>
#include "board.h"
#include "dual_network_board.h"
#include "audio_codec.h"
#include "at_modem.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <vector>
LV_FONT_DECLARE(font_puhui_16_1);
namespace {
constexpr const char* TAG="Radio";
using radio::stations;
using radio::station_count;
using radio::regions;
using radio::region_count;
using radio::genres;
using radio::genre_count;
using radio::StreamKind;
struct AudioSegment { unsigned generation; esp_audio_simple_dec_type_t type; std::vector<uint8_t> data; bool reset_decoder=false; };
class RadioApp;
std::atomic<RadioApp*> active_radio{nullptr};
class RadioApp {
    Board& board_=Board::GetInstance();
    AudioCodec* codec_=nullptr;
    QueueHandle_t queue_=nullptr;
    std::atomic<unsigned> generation_{1}, station_{0};
    std::atomic<uint64_t> downloaded_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<int> volume_{25};
    std::atomic<AtModem*> modem_{nullptr};
    std::atomic<const char*> network_message_{nullptr};
    std::atomic<bool> power_blocked_{false},shutting_down_{false},ready_{false};
    lv_obj_t *station_label_=nullptr,*status_label_=nullptr,*data_label_=nullptr;
    lv_obj_t *play_label_=nullptr,*volume_label_=nullptr,*region_label_=nullptr;
    lv_obj_t *player_=nullptr,*browser_=nullptr,*network_label_=nullptr,*battery_label_=nullptr;
    lv_obj_t *battery_fill_=nullptr,*charge_sign_=nullptr,*power_label_=nullptr;
    lv_obj_t *signal_bars_[4]={},*station_rows_[3]={},*row_labels_[3]={};
    lv_obj_t *filter_label_=nullptr,*genre_menu_=nullptr,*page_menu_=nullptr,*back_label_=nullptr,*empty_label_=nullptr;
    std::vector<unsigned> filtered_;
    unsigned filter_region_=0,filter_genre_=0,page_=0,page_options_count_=0;
    bool browser_used_=false;
    std::string status_text_;
    uint64_t usage_tenths_=0;
    void UpdateLabel(lv_obj_t* label,const char* text) {
        if(strcmp(lv_label_get_text(label),text)!=0)lv_label_set_text(label,text);
    }
    bool Active(unsigned g) const { return enabled_.load()&&!power_blocked_.load()&&generation_.load()==g; }
    void Status(const char* message,unsigned g=0) {
        if(g&&g!=generation_.load())return;
        if(lvgl_port_lock(1000)) {
            if(g&&g!=generation_.load()){lvgl_port_unlock();return;}
            if(status_text_!=message) { status_text_=message;lv_label_set_text(status_label_,message);ESP_LOGI(TAG,"%s",message); }
            lvgl_port_unlock();
        }
    }
    void UpdateUsage() {
        uint64_t value=downloaded_.load()/100000;
        if(value==usage_tenths_)return;
        if(lvgl_port_lock(1000)) {
            usage_tenths_=value;
            char text[64];snprintf(text,sizeof(text),"%.1fMB",downloaded_.load()/1000000.0);
            UpdateLabel(data_label_,text);lvgl_port_unlock();
        }
    }
    void StationChanged() {
        generation_.fetch_add(1);
        const auto& s=stations[station_.load()];
        lv_label_set_text(station_label_,s.name);
        if(s.high_data)lv_label_set_text_fmt(region_label_,"%s · 高流量",s.region);
        else lv_label_set_text_fmt(region_label_,"%s · %s",s.region,s.genre);
        status_text_=enabled_.load()?"正在切换电台...":"已暂停 · 点击播放";
        lv_label_set_text(status_label_,status_text_.c_str());
    }
    void Change(int direction) {
        auto found=std::find(filtered_.begin(),filtered_.end(),station_.load());
        if(browser_used_&&!filtered_.empty()&&found!=filtered_.end()) {
            int next=(int(found-filtered_.begin())+int(filtered_.size())+direction)%int(filtered_.size());
            station_=filtered_[next];
        } else station_=unsigned((int(station_.load())+int(station_count)+direction)%int(station_count));
        StationChanged();
    }
    void Toggle() {
        if(shutting_down_.load())return;
        if(!ready_.load()){lv_label_set_text(status_label_,"电台尚未就绪，请等待或重启");return;}
        if(power_blocked_.load()) { status_text_="电量或电源保护，请先检查充电";lv_label_set_text(status_label_,status_text_.c_str());return; }
        bool value=!enabled_.load();enabled_=value;generation_.fetch_add(1);
        lv_label_set_text(play_label_,value?"暂停":"播放");
        status_text_=value?"正在缓冲...":"已暂停 · 点击播放";
        lv_label_set_text(status_label_,status_text_.c_str());
    }
    void Volume(int delta) {
        int value=std::max(0,std::min(80,volume_.load()+delta));volume_=value;
        lv_label_set_text_fmt(volume_label_,"音量%d",value);
    }
    lv_obj_t* Label(lv_obj_t* parent,const char* text,int x,int y,int w,int h) {
        auto obj=lv_label_create(parent);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
        lv_label_set_text(obj,text);return obj;
    }
    lv_obj_t* Box(lv_obj_t* parent,int x,int y,int w,int h,int border=0) {
        auto obj=lv_obj_create(parent);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
        lv_obj_set_style_pad_all(obj,0,0);lv_obj_set_style_border_width(obj,border,0);
        lv_obj_set_style_border_color(obj,lv_color_black(),0);
        lv_obj_set_style_bg_color(obj,lv_color_white(),0);lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);
        lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);return obj;
    }
    lv_obj_t* Button(lv_obj_t* parent,const char* text,int action,int x,int y,int w,int h,lv_obj_t** label_out=nullptr) {
        auto button=lv_button_create(parent);lv_obj_set_pos(button,x,y);lv_obj_set_size(button,w,h);
        lv_obj_set_style_pad_all(button,0,0);lv_obj_set_style_radius(button,5,0);
        lv_obj_set_style_bg_color(button,lv_color_white(),0);lv_obj_set_style_text_color(button,lv_color_black(),0);
        lv_obj_set_style_border_width(button,1,0);lv_obj_set_style_border_color(button,lv_color_black(),0);
        auto label=lv_label_create(button);lv_obj_set_width(label,w-4);
        lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(label,text);lv_obj_center(label);
        if(label_out)*label_out=label;
        lv_obj_set_user_data(button,reinterpret_cast<void*>(intptr_t(action)));
        lv_obj_add_event_cb(button,[](lv_event_t* e){
            auto self=static_cast<RadioApp*>(lv_event_get_user_data(e));
            int action=int(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
            if(action==-1||action==1)self->Change(action);
            else if(action==0)self->Toggle();
            else if(action==2||action==3)self->Volume(action==2?-5:5);
            else if(action==4)self->OpenBrowser();
            else if(action==5)self->CloseBrowser();
            else if(action==8||action==9)self->ChangePage(action==8?-1:1);
            else if(action>=10&&action<13)self->SelectRow(action-10);
            else if(action==15)self->RequestShutdown();
        },LV_EVENT_CLICKED,this);
        return button;
    }
    lv_obj_t* Dropdown(lv_obj_t* parent,int x,int y,int width,int height,const std::string& options,int kind) {
        auto menu=lv_dropdown_create(parent);lv_obj_set_pos(menu,x,y);lv_obj_set_size(menu,width,height);
        lv_dropdown_set_options(menu,options.c_str());lv_dropdown_set_symbol(menu,kind==22?nullptr:"v");
        lv_obj_set_style_pad_all(menu,2,0);lv_obj_set_style_bg_color(menu,lv_color_white(),0);
        lv_obj_set_style_text_color(menu,lv_color_black(),0);lv_obj_set_style_border_color(menu,lv_color_black(),0);
        auto list=lv_dropdown_get_list(menu);lv_obj_set_style_text_font(list,&font_puhui_16_1,0);
        lv_obj_set_style_max_height(list,176,0);lv_obj_set_style_bg_color(list,lv_color_white(),0);
        lv_obj_set_style_text_color(list,lv_color_black(),0);lv_obj_set_style_bg_color(list,lv_color_black(),LV_PART_SELECTED);
        lv_obj_set_style_text_color(list,lv_color_white(),LV_PART_SELECTED);
        if(kind==22)lv_dropdown_set_dir(menu,LV_DIR_TOP);
        lv_obj_set_user_data(menu,reinterpret_cast<void*>(intptr_t(kind)));
        lv_obj_add_event_cb(menu,[](lv_event_t* e){auto self=static_cast<RadioApp*>(lv_event_get_user_data(e));
            auto target=lv_event_get_target_obj(e);int kind=int(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
            unsigned selected=lv_dropdown_get_selected(target);
            if(kind==20){self->filter_region_=selected;self->Filter();}
            else if(kind==21){self->filter_genre_=selected;self->Filter();}
            else self->page_=selected;
            self->BrowserPage();
        },LV_EVENT_VALUE_CHANGED,this);
        return menu;
    }
    void Filter() {
        filtered_.clear();
        for(unsigned i=0;i<station_count;i++) {
            bool region=filter_region_==0||strcmp(stations[i].region,regions[filter_region_])==0;
            bool genre=filter_genre_==0||strcmp(stations[i].genre,genres[filter_genre_])==0;
            if(region&&genre)filtered_.push_back(i);
        }
        page_=0;
    }
    void BrowserPage() {
        lv_dropdown_set_selected(filter_label_,filter_region_);
        lv_dropdown_set_text(filter_label_,filter_region_?regions[filter_region_]:"地区");
        lv_dropdown_set_selected(genre_menu_,filter_genre_);
        lv_dropdown_set_text(genre_menu_,filter_genre_?genres[filter_genre_]:"类型");
        const unsigned pages=std::max(1u,unsigned((filtered_.size()+2)/3));
        page_=std::min(page_,pages-1);
        if(page_options_count_!=pages){std::string options;for(unsigned i=1;i<=pages;i++){if(i>1)options+='\n';options+=std::to_string(i);}lv_dropdown_set_options(page_menu_,options.c_str());page_options_count_=pages;}
        lv_dropdown_set_selected(page_menu_,page_);
        char page_text[24];snprintf(page_text,sizeof(page_text),"%u/%u",filtered_.empty()?0:page_+1,filtered_.empty()?0:pages);
        lv_dropdown_set_text(page_menu_,page_text);
        lv_label_set_text_fmt(back_label_,"返回播放 %u台",unsigned(filtered_.size()));
        if(filtered_.empty())lv_obj_remove_flag(empty_label_,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(empty_label_,LV_OBJ_FLAG_HIDDEN);
        for(unsigned row=0;row<3;row++) {
            unsigned pos=page_*3+row;
            if(pos>=filtered_.size()) { lv_obj_add_flag(station_rows_[row],LV_OBJ_FLAG_HIDDEN);continue; }
            lv_obj_remove_flag(station_rows_[row],LV_OBJ_FLAG_HIDDEN);
            unsigned index=filtered_[pos];lv_label_set_text(row_labels_[row],stations[index].name);
            bool selected=index==station_.load();
            lv_obj_set_style_bg_color(station_rows_[row],selected?lv_color_black():lv_color_white(),0);
            lv_obj_set_style_text_color(station_rows_[row],selected?lv_color_white():lv_color_black(),0);
        }
    }
    void OpenBrowser() {
        if(!browser_used_) {
            filter_region_=0;filter_genre_=0;
            for(unsigned i=1;i<region_count;i++)if(strcmp(regions[i],stations[station_.load()].region)==0){filter_region_=i;break;}
            Filter();browser_used_=true;
        }
        auto found=std::find(filtered_.begin(),filtered_.end(),station_.load());
        if(found!=filtered_.end())page_=unsigned(found-filtered_.begin())/3;
        BrowserPage();lv_obj_add_flag(player_,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(browser_,LV_OBJ_FLAG_HIDDEN);
    }
    void CloseBrowser() { lv_obj_add_flag(browser_,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(player_,LV_OBJ_FLAG_HIDDEN); }
    void ChangePage(int direction) {
        unsigned pages=std::max(1u,unsigned((filtered_.size()+2)/3));
        page_=unsigned((int(page_)+int(pages)+direction)%int(pages));BrowserPage();
    }
    void SelectRow(unsigned row) {
        unsigned pos=page_*3+row;if(pos>=filtered_.size())return;
        station_=filtered_[pos];StationChanged();CloseBrowser();
    }
    bool Ui() {
        if(!lvgl_port_lock(3000))return false;
        auto screen=Box(lv_layer_top(),0,0,176,264);
        lv_obj_set_style_radius(screen,0,0);lv_obj_set_style_text_color(screen,lv_color_black(),0);
        lv_obj_set_style_text_font(screen,&font_puhui_16_1,0);
        auto header=Box(screen,0,0,176,26);lv_obj_set_style_radius(header,0,0);
        for(int i=0;i<4;i++){int height=3+i*3;signal_bars_[i]=Box(header,8+i*5,19-height,3,height,1);}
        network_label_=Label(header,"4G",32,3,68,20);
        auto battery=Box(header,106,8,20,11,1);lv_obj_set_style_radius(battery,1,0);
        battery_fill_=Box(battery,2,2,1,5);lv_obj_set_style_bg_color(battery_fill_,lv_color_black(),0);
        auto cap=Box(header,126,11,2,5);lv_obj_set_style_bg_color(cap,lv_color_black(),0);
        battery_label_=Label(header,"--%",131,3,44,20);
        charge_sign_=Label(header,"+",94,3,12,20);lv_obj_add_flag(charge_sign_,LV_OBJ_FLAG_HIDDEN);
        auto rule=Box(screen,8,26,160,1);lv_obj_set_style_bg_color(rule,lv_color_black(),0);
        player_=Box(screen,0,30,176,234);lv_obj_set_style_radius(player_,0,0);
        region_label_=Label(player_,"全国",8,2,160,20);
        auto card=Box(player_,8,24,160,54,2);lv_obj_set_style_radius(card,7,0);
        station_label_=Label(card,stations[0].name,5,5,146,40);lv_obj_set_style_text_align(station_label_,LV_TEXT_ALIGN_CENTER,0);
        lv_label_set_long_mode(station_label_,LV_LABEL_LONG_DOT);
        status_label_=Label(player_,"正在连接 4G...",8,82,160,20);lv_label_set_long_mode(status_label_,LV_LABEL_LONG_DOT);
        Button(player_,"上台",-1,8,106,49,32);
        auto play=Button(player_,"播放",0,62,106,52,32,&play_label_);
        lv_obj_set_style_bg_color(play,lv_color_black(),0);lv_obj_set_style_text_color(play,lv_color_white(),0);
        Button(player_,"下台",1,119,106,49,32);
        Button(player_,"-",2,8,144,32,28);volume_label_=Label(player_,"音量25",44,148,88,20);
        lv_obj_set_style_text_align(volume_label_,LV_TEXT_ALIGN_CENTER,0);Button(player_,"+",3,136,144,32,28);
        Button(player_,"分类选台",4,8,178,106,30);Button(player_,"电源",15,120,178,48,30);
        lv_obj_add_flag(card,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card,[](lv_event_t* e){static_cast<RadioApp*>(lv_event_get_user_data(e))->OpenBrowser();},LV_EVENT_CLICKED,this);
        power_label_=Label(player_,"读取电量",8,214,82,20);data_label_=Label(player_,"0.0MB",92,214,76,20);
        browser_=Box(screen,8,32,160,228);lv_obj_set_style_radius(browser_,0,0);
        std::string region_options;for(unsigned i=0;i<region_count;i++){if(i)region_options+='\n';region_options+=regions[i];}
        std::string genre_options;for(unsigned i=0;i<genre_count;i++){if(i)genre_options+='\n';genre_options+=genres[i];}
        filter_label_=Dropdown(browser_,0,0,78,32,region_options,20);
        genre_menu_=Dropdown(browser_,82,0,78,32,genre_options,21);
        for(unsigned row=0;row<3;row++) {
            station_rows_[row]=Button(browser_,"",10+row,0,36+row*44,160,42,&row_labels_[row]);
            lv_obj_set_height(row_labels_[row],40);lv_label_set_long_mode(row_labels_[row],LV_LABEL_LONG_DOT);
        }
        empty_label_=Label(browser_,"该分类暂无电台",4,80,152,40);lv_obj_set_style_text_align(empty_label_,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_add_flag(empty_label_,LV_OBJ_FLAG_HIDDEN);
        Button(browser_,"上页",8,0,170,44,26);page_menu_=Dropdown(browser_,46,170,68,26,"1",22);
        Button(browser_,"下页",9,116,170,44,26);Button(browser_,"返回播放",5,0,200,160,28,&back_label_);
        lv_obj_add_flag(browser_,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(region_label_,"%s · %s",stations[0].region,stations[0].genre);
        lvgl_port_unlock();return true;
    }
    void PowerAndSignal() {
        unsigned ticks=0,critical_samples=0,invalid_samples=0;unsigned previous_power=~0u;
        while(true) {
            if(shutting_down_.load()){vTaskDelay(pdMS_TO_TICKS(200));continue;}
            if(auto message=network_message_.exchange(nullptr))Status(message);
            BoardPowerStatus power;bool valid=board_.GetPowerStatus(power);
            if(valid) {
                invalid_samples=0;
                bool critical_fault=(power.fault_flags&0x18)!=0;
                if(critical_fault||(!power.external_power&&power.millivolts<=3400)) {
                    power_blocked_=true;
                    if(enabled_.exchange(false)) {generation_.fetch_add(1);if(lvgl_port_lock(1000)){lv_label_set_text(play_label_,"播放");lvgl_port_unlock();}}
                    Status(critical_fault?"电源保护，请检查充电":"电量过低，请连接充电");
                } else if(power.external_power||power.millivolts>=3550)power_blocked_=false;
                if(!power.external_power&&power.millivolts<=3400)++critical_samples;else critical_samples=0;
                if(critical_samples>=3&&!shutting_down_.load())RequestShutdown();
            } else if(++invalid_samples>=3) {
                power_blocked_=true;
                if(enabled_.exchange(false)){generation_.fetch_add(1);if(lvgl_port_lock(1000)){lv_label_set_text(play_label_,"播放");lvgl_port_unlock();}}
                Status("电量读取异常，已停止播放");
            }
            unsigned power_code=(valid?1u:0u)|(power.external_power?2u:0u)|(power.charging?4u:0u)|(power.full?8u:0u)|(power.fault_flags<<4);
            if(ticks%6==0||power_code!=previous_power) {
                ESP_LOGI(TAG,"Power valid=%d SOC=%d mv=%d ma=%d external=%d charging=%d full=%d fault=0x%x FCC=%d estimated=%d",valid,power.percent,power.millivolts,power.milliamps,power.external_power,power.charging,power.full,power.fault_flags,power.full_capacity_mah,power.estimated);
                previous_power=power_code;
            }
            if(lvgl_port_lock(1000)) {
                if(valid) {
                    char battery_text[20];snprintf(battery_text,sizeof(battery_text),"%s%d%%",power.estimated?"~":"",power.percent);
                    UpdateLabel(battery_label_,battery_text);
                    int width=power.percent*16/100;
                    if(width>0){lv_obj_set_width(battery_fill_,width);lv_obj_remove_flag(battery_fill_,LV_OBJ_FLAG_HIDDEN);}else lv_obj_add_flag(battery_fill_,LV_OBJ_FLAG_HIDDEN);
                    if(power.charging)lv_obj_remove_flag(charge_sign_,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(charge_sign_,LV_OBJ_FLAG_HIDDEN);
                    const char* state=power.fault_flags?"充电异常":power.thermal_regulation?"充电温控":power.input_limited?"供电受限":power.full?"已充满":power.charging?"充电中":power.external_power?"外接电源":"电池供电";
                    char power_text[80];snprintf(power_text,sizeof(power_text),"%s",state);
                    UpdateLabel(power_label_,power_text);
                } else {UpdateLabel(battery_label_,"--%");UpdateLabel(power_label_,"电量异常");lv_obj_add_flag(battery_fill_,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(charge_sign_,LV_OBJ_FLAG_HIDDEN);}
                lvgl_port_unlock();
            }
            ++ticks;vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    void SignalMonitor() {
        unsigned ticks=0;AtModem* last_modem=nullptr;bool last_ready=false;
        while(true) {
            if(!shutting_down_.load()) {
            auto modem=modem_.load();
            if(modem&&(modem!=last_modem||ticks%30==0||modem->network_ready()!=last_ready)) {
                bool ready=modem->network_ready();last_modem=modem;last_ready=ready;int csq=ready?modem->GetCsq():-1;
                int bars=csq<0||csq>31?0:csq>=20?4:csq>=15?3:csq>=10?2:1;
                ESP_LOGI(TAG,"Signal ready=%d csq=%d bars=%d",ready,csq,bars);
                if(lvgl_port_lock(1000)) {
                    UpdateLabel(network_label_,!ready?"离线":csq<0||csq>31?"4G ?":"4G");
                    for(int i=0;i<4;i++)lv_obj_set_style_bg_color(signal_bars_[i],i<bars?lv_color_black():lv_color_white(),0);
                    lvgl_port_unlock();
                }
            }
            }
            ++ticks;vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    std::unique_ptr<Http> OpenHttp(std::string url,unsigned g,bool streaming,std::string* resolved=nullptr) {
        for(int hop=0;hop<4&&Active(g);hop++) {
            std::unique_ptr<Http> http=streaming?std::unique_ptr<Http>(new HttpClient(board_.GetNetwork(),4)):board_.GetNetwork()->CreateHttp(4);
            http->SetTimeout(12000);http->SetHeader("User-Agent","XiaoZhiCard-Radio/2.0");
            http->SetHeader("Accept-Encoding","identity");http->SetHeader("Icy-MetaData","0");
            if(!http->Open("GET",url)){http->Close();return nullptr;}
            int code=http->GetStatusCode();
            if(code==200){if(resolved)*resolved=url;return http;}
            if(code==301||code==302||code==303||code==307||code==308) {
                auto location=http->GetResponseHeader("location");http->Close();
                if(location.empty())return nullptr;
                url=radio::Resolve(url,location);continue;
            }
            ESP_LOGW(TAG,"HTTP status %d",code);http->Close();return nullptr;
        }
        return nullptr;
    }
    bool Fetch(const std::string& url,size_t limit,unsigned g,std::vector<uint8_t>& data,std::string* resolved=nullptr) {
        auto http=OpenHttp(url,g,false,resolved);if(!http)return false;
        char buffer[4096];data.clear();bool ok=true;
        while(Active(g)) {
            int n=http->Read(buffer,sizeof(buffer));if(n<0){ok=false;break;}if(n==0)break;
            downloaded_.fetch_add(n);
            if(data.size()+n>limit){ok=false;break;}
            data.insert(data.end(),buffer,buffer+n);
        }
        http->Close();UpdateUsage();return ok&&Active(g)&&!data.empty();
    }
    bool SendAudio(AudioSegment* audio,unsigned g) {
        while(Active(g))if(xQueueSend(queue_,&audio,pdMS_TO_TICKS(100))==pdTRUE)return true;
        delete audio;return false;
    }
    void StreamAudio(const std::string& url,unsigned g,esp_audio_simple_dec_type_t type) {
        auto http=OpenHttp(url,g,true);
        if(!http){Status("直播连接失败，正在重试",g);return;}
        unsigned interval=unsigned(strtoul(http->GetResponseHeader("icy-metaint").c_str(),nullptr,10));
        if(interval>16*1024*1024){http->Close();Status("直播元数据格式暂不支持",g);return;}
        unsigned until_meta=interval,skip_meta=0;char buffer[4096];
        auto audio=new AudioSegment{g,type,{}};audio->data.reserve(8192);
        while(Active(g)) {
            int count=http->Read(buffer,sizeof(buffer));if(count<=0)break;
            downloaded_.fetch_add(count);
            for(int i=0;i<count&&Active(g);i++) {
                unsigned char value=buffer[i];
                if(skip_meta){--skip_meta;continue;}
                if(interval&&until_meta==0){skip_meta=value*16;until_meta=interval;continue;}
                if(interval)--until_meta;
                audio->data.push_back(value);
                if(audio->data.size()>=8192) {
                    if(!SendAudio(audio,g)){audio=nullptr;break;}
                    audio=new AudioSegment{g,type,{}};audio->data.reserve(8192);
                }
            }
            UpdateUsage();
        }
        if(audio) {
            if(!audio->data.empty()&&Active(g))SendAudio(audio,g);else delete audio;
        }
        http->Close();
    }
    bool MediaPlaylist(const std::string& source,unsigned g,std::vector<radio::Segment>& entries) {
        std::string url=source;
        for(int depth=0;depth<3;depth++) {
            std::vector<uint8_t> data;std::string actual;
            if(!Fetch(url,32768,g,data,&actual))return false;
            url=actual;
            std::string text(data.begin(),data.end());
            if(text.find("#EXT-X-STREAM-INF")==std::string::npos)return radio::Parse(url,text,entries);
            std::istringstream lines(text);std::string line;bool selected=false;
            while(std::getline(lines,line)) {
                while(!line.empty()&&(line.back()=='\r'||line.back()==' '||line.back()=='\t'))line.pop_back();
                if(!line.empty()&&line[0]!='#'){url=radio::Resolve(url,line);selected=true;break;}
            }
            if(!selected)return false;
        }
        return false;
    }
    void Download() {
        unsigned current=0;uint64_t next=0;bool have_next=false;
        std::string last_url;TickType_t last_audio=xTaskGetTickCount();
        while(true) {
            if(!enabled_.load()||power_blocked_.load()) { vTaskDelay(pdMS_TO_TICKS(200));continue; }
            unsigned g=generation_.load();if(g!=current){current=g;have_next=false;last_url.clear();last_audio=xTaskGetTickCount();}
            auto modem=modem_.load();
            if(!modem||!modem->network_ready()) { Status("4G 已断开，等待重连",g);vTaskDelay(pdMS_TO_TICKS(2000));continue; }
            const auto selected=stations[station_.load()];
            if(selected.kind==StreamKind::Mp3||selected.kind==StreamKind::Aac) {
                StreamAudio(selected.url,g,selected.kind==StreamKind::Mp3?ESP_AUDIO_SIMPLE_DEC_TYPE_MP3:ESP_AUDIO_SIMPLE_DEC_TYPE_AAC);
                if(Active(g)){generation_.fetch_add(1);Status("直播中断，正在重新连接");}
                vTaskDelay(pdMS_TO_TICKS(2000));continue;
            }
            std::vector<radio::Segment> entries;
            if(!MediaPlaylist(selected.url,g,entries)) { if(Active(g))Status("连接失败或格式不支持，正在重试",g);vTaskDelay(pdMS_TO_TICKS(2000));continue; }
            if(have_next&&entries.back().sequenced&&entries.back().sequence+1<next) {
                generation_.fetch_add(1);Status("直播已更新，正在重新缓冲");continue;
            }
            if(xTaskGetTickCount()-last_audio>pdMS_TO_TICKS(45000)){generation_.fetch_add(1);continue;}
            if(!have_next){next=entries[entries.size()>1?entries.size()-2:0].sequence;have_next=true;}
            auto type=selected.kind==StreamKind::HlsAac?ESP_AUDIO_SIMPLE_DEC_TYPE_AAC:selected.kind==StreamKind::HlsMp3?ESP_AUDIO_SIMPLE_DEC_TYPE_MP3:ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
            bool after_last=last_url.empty();
            bool last_present=std::any_of(entries.begin(),entries.end(),[&](const radio::Segment& e){return e.url==last_url;});
            for(const auto& entry:entries) {
                if(!Active(g))break;
                if(entry.sequenced) { if(entry.sequence<next)continue; }
                else if(last_present&&!after_last){if(entry.url==last_url)after_last=true;continue;}
                auto audio=new AudioSegment{g,type,{},entry.discontinuity};
                if(!Fetch(entry.url,512*1024,g,audio->data)) { delete audio;Status("音频下载中断，正在重试",g);break; }
                if(!SendAudio(audio,g))break;
                next=entry.sequence+1;last_url=entry.url;last_audio=xTaskGetTickCount();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    void Play() {
        while(!ready_.load())vTaskDelay(pdMS_TO_TICKS(10));
        esp_audio_simple_dec_handle_t decoder=nullptr;unsigned current=0;int last_rate=0,last_volume=-1;
        int phase=0,previous=0;bool have_previous=false;
        std::vector<uint8_t> output(32768);
        while(true) {
            int volume=volume_.load();if(volume!=last_volume){codec_->SetOutputVolume(volume);last_volume=volume;}
            AudioSegment* item=nullptr;if(xQueueReceive(queue_,&item,pdMS_TO_TICKS(100))!=pdTRUE)continue;
            if(!Active(item->generation)){delete item;continue;}
            if(current!=item->generation||item->reset_decoder) {
                if(decoder)esp_audio_simple_dec_close(decoder);
                decoder=nullptr;current=item->generation;last_rate=0;
                esp_ts_dec_cfg_t ts={};ts.aac_plus_enable=true;
                esp_aac_dec_cfg_t aac={};aac.aac_plus_enable=true;
                esp_audio_simple_dec_cfg_t cfg={};cfg.dec_type=item->type;
                if(item->type==ESP_AUDIO_SIMPLE_DEC_TYPE_TS){cfg.dec_cfg=&ts;cfg.cfg_size=sizeof(ts);}
                else if(item->type==ESP_AUDIO_SIMPLE_DEC_TYPE_AAC){cfg.dec_cfg=&aac;cfg.cfg_size=sizeof(aac);}
                if(esp_audio_simple_dec_open(&cfg,&decoder)!=ESP_AUDIO_ERR_OK) { Status("音频解码器启动失败",current);current=0;delete item;continue; }
            }
            size_t offset=0;bool first=true;
            while(decoder&&offset<item->data.size()&&Active(current)) {
                int requested_volume=volume_.load();
                if(requested_volume!=last_volume){codec_->SetOutputVolume(requested_volume);last_volume=requested_volume;}
                esp_audio_simple_dec_raw_t raw={};raw.buffer=item->data.data()+offset;raw.len=item->data.size()-offset;
                esp_audio_simple_dec_out_t frame={};frame.buffer=output.data();frame.len=output.size();
                auto result=esp_audio_simple_dec_process(decoder,&raw,&frame);
                if(result==ESP_AUDIO_ERR_BUFF_NOT_ENOUGH&&frame.needed_size>output.size()&&frame.needed_size<=131072) {
                    output.resize(frame.needed_size);continue;
                }
                if((result!=ESP_AUDIO_ERR_OK&&result!=ESP_AUDIO_ERR_CONTINUE)||raw.consumed>raw.len) {
                    Status("音频解码失败，等待下一段",current);
                    esp_audio_simple_dec_close(decoder);decoder=nullptr;current=0;break;
                }
                offset+=raw.consumed;
                if(frame.decoded_size) {
                    esp_audio_simple_dec_info_t info={};
                    if(esp_audio_simple_dec_get_info(decoder,&info)!=ESP_AUDIO_ERR_OK||info.bits_per_sample!=16||
                       (info.channel!=1&&info.channel!=2)||info.sample_rate<8000||info.sample_rate>48000) { Status("暂不支持该音频格式",current);esp_audio_simple_dec_close(decoder);decoder=nullptr;current=0;break; }
                    size_t samples=frame.decoded_size/sizeof(int16_t)/info.channel;
                    auto pcm=reinterpret_cast<const int16_t*>(output.data());std::vector<int16_t> mono(samples);
                    for(size_t i=0;i<samples;i++)mono[i]=info.channel==1?pcm[i]:int16_t((int(pcm[i*2])+pcm[i*2+1])/2);
                    if(first){ESP_LOGI(TAG,"PCM decoded: %lu Hz, %u channels",(unsigned long)info.sample_rate,info.channel);Status("正在播放",current);first=false;}
                    if(int(info.sample_rate)!=codec_->output_sample_rate()) {
                        if(last_rate!=int(info.sample_rate)){phase=0;have_previous=false;last_rate=info.sample_rate;}
                        const int out_rate=codec_->output_sample_rate();
                        std::vector<int16_t> converted;converted.reserve(mono.size()*out_rate/info.sample_rate+3);
                        for(int sample:mono) {
                            if(have_previous) {
                                while(phase<out_rate) {
                                    converted.push_back(int16_t(previous+int64_t(sample-previous)*phase/out_rate));
                                    phase+=info.sample_rate;
                                }
                                phase-=out_rate;
                            }
                            previous=sample;have_previous=true;
                        }
                        if(Active(current)&&!converted.empty())codec_->OutputData(converted);
                    } else if(Active(current)) { last_rate=info.sample_rate;codec_->OutputData(mono); }
                }
                if(!raw.consumed&&!frame.decoded_size)break;
            }
            delete item;
        }
    }
public:
    void RequestShutdown() {
        if(shutting_down_.exchange(true))return;
        enabled_=false;generation_.fetch_add(1);
        if(lvgl_port_lock(1000)){lv_label_set_text(play_label_,"播放");lvgl_port_unlock();}
        Status("正在关机");
        if(!board_.PowerOff())CancelShutdown("关机请求失败，请重试");
    }
    void CancelShutdown(const char* message) {
        shutting_down_=false;
        Status(message);
    }

    void ShowPoweredOff() {
        shutting_down_=true;
        if(lvgl_port_lock(3000)) {
            CloseBrowser();lv_label_set_text(station_label_,"已停止运行");lv_label_set_text(status_label_,"长按背键开机");
            lv_label_set_text(network_label_,"关机");lv_label_set_text(power_label_,"长按开机");
            for(auto bar:signal_bars_)lv_obj_set_style_bg_color(bar,lv_color_white(),0);
            lv_obj_invalidate(lv_layer_top());lv_refr_now(nullptr);lvgl_port_unlock();
        }
    }
    void ToggleFromButton() {
        if(lvgl_port_lock(1000)) { if(play_label_&&status_label_)Toggle();lvgl_port_unlock(); }
    }
    void Start() {
        if(!Ui()){ESP_LOGE(TAG,"Radio UI lock failed");return;}
        queue_=xQueueCreate(3,sizeof(AudioSegment*));
        if(!queue_){Status("播放缓冲区创建失败");return;}
        auto& dual=static_cast<DualNetworkBoard&>(board_);
        static_cast<Ml307Board&>(dual.GetCurrentBoard()).SetConnectionStatusCallback([this](const char* text){network_message_=text;});
        if(xTaskCreate([](void* p){static_cast<RadioApp*>(p)->PowerAndSignal();},"radio_power",6144,this,3,nullptr)!=pdPASS){Status("电量监控启动失败");return;}
        if(xTaskCreate([](void* p){static_cast<RadioApp*>(p)->SignalMonitor();},"radio_signal",4096,this,2,nullptr)!=pdPASS){Status("信号监控启动失败，请重启");return;}
        board_.StartNetwork();modem_=static_cast<AtModem*>(board_.GetNetwork());codec_=board_.GetAudioCodec();codec_->Start();codec_->EnableInput(false);
        volume_=std::max(0,std::min(80,codec_->output_volume()));
        if(lvgl_port_lock(1000)){lv_label_set_text_fmt(volume_label_,"音量%d",volume_.load());lvgl_port_unlock();}
        if(esp_aac_dec_register()!=ESP_AUDIO_ERR_OK||esp_mp3_dec_register()!=ESP_AUDIO_ERR_OK||esp_ts_dec_register()!=ESP_AUDIO_ERR_OK){Status("音频组件初始化失败");return;}
        bool ready=static_cast<AtModem*>(board_.GetNetwork())->network_ready();
        Status(ready?(enabled_.load()?"正在缓冲...":"4G 已就绪，点击播放"):"4G 暂未就绪，播放时重试");
        TaskHandle_t audio_task=nullptr;
        if(xTaskCreate([](void* p){static_cast<RadioApp*>(p)->Play();},"radio_audio",24576,this,5,&audio_task)!=pdPASS ||
           xTaskCreate([](void* p){static_cast<RadioApp*>(p)->Download();},"radio_fetch",12288,this,4,nullptr)!=pdPASS){if(audio_task)vTaskDelete(audio_task);Status("电台服务启动失败，请重启");return;}
        ready_=true;
    }
};
}
void StartRadioApp() {
#ifdef RADIO_RUN_SELF_TESTS
    RadioSelfTest();ESP_LOGI("RadioTest","playlist regression checks passed");
#endif
    static RadioApp app;active_radio=&app;app.Start();
}
void ToggleRadioPlayback() { auto app=active_radio.load();if(app)app->ToggleFromButton(); }

void RequestRadioShutdown() { auto app=active_radio.load();if(app)app->RequestShutdown(); }

void ShowRadioPoweredOff() { auto app=active_radio.load();if(app)app->ShowPoweredOff(); }

void CancelRadioShutdown(const char* message) {auto app=active_radio.load();if(app)app->CancelShutdown(message);}
