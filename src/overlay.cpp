#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <sys/stat.h>
#include "protocol.hpp"
#include "overlay_storage.hpp"
#include "ui_policy.hpp"

namespace {
lr::OverlayStorage storage;
Result launchServiceResult = 0;
const char* overlayStatusPath = "sdmc:/config/lite-recorder/status.bin";
const char* overlayCommandPath = "sdmc:/config/lite-recorder/command.bin";
uint64_t nowUs() { return armTicksToNs(armGetSystemTick()) / 1000; }
bool running() { u64 pid = 0; return R_SUCCEEDED(pmdmntGetProcessId(&pid, lr::ModuleId)) && pid; }
const char* stateLabel(lr::State s) {
    switch (s) {
    case lr::State::Idle: return "待机";
    case lr::State::Waiting: return "等待游戏关键帧";
    case lr::State::Recording: return "录制中";
    case lr::State::Saved: return "录像已保存";
    case lr::State::Error: return "录制停止：有错误";
    case lr::State::Stalled: return "等待取流恢复（未结束）";
    case lr::State::Paused: return "已暂停";
    case lr::State::NeedCodec: return "等待视频参数";
    }
    return "未知";
}
const char* faultLabel(lr::Fault f) {
    switch (f) {
    case lr::Fault::None: return "无";
    case lr::Fault::Capture: return "取流失败：请记录错误码";
    case lr::Fault::QueueFull: return "数据队列已满";
    case lr::Fault::Disk: return "空间不足或文件达到安全上限";
    case lr::Fault::File: return "文件读写失败";
    case lr::Fault::NoVideo: return "没有收到视频";
    case lr::Fault::Thread: return "后台线程启动失败";
    case lr::Fault::Conflict: return "请停用原录屏 / SysDVR 后重启";
    case lr::Fault::Timestamp: return "游戏时间戳已重置";
    case lr::Fault::Stalled: return "取流中断，请保留测试目录";
    case lr::Fault::Clock: return "计时异常，请保留测试目录";
    case lr::Fault::Codec: return "视频格式不受支持，请保留目录";
    }
    return "未知错误";
}
std::string sizeString(uint64_t bytes) {
    char b[48];
    if(bytes>=1024ULL*1024*1024) snprintf(b,sizeof(b),"%.2f GiB",double(bytes)/(1024*1024*1024));
    else snprintf(b,sizeof(b),"%.2f MiB",double(bytes)/(1024*1024));
    return b;
}
// Keep the row allocated so changing visibility never deletes a focused item.
class StopItem : public tsl::elm::ListItem {
    bool shown=false;
public:
    StopItem():ListItem("停止并保存") {}
    bool visible() const { return shown; }
    void show(bool value) {
        if(shown==value) return;
        shown=value;
        setBoundaries(getX(),getY(),getWidth(),shown?tsl::style::ListItemDefaultHeight:0);
        if(getParent()) getParent()->invalidate();
    }
    void layout(u16 x,u16 y,u16 w,u16 h) override {
        ListItem::layout(x,y,w,h);
        if(!shown) setBoundaries(getX(),getY(),getWidth(),0);
    }
    void draw(tsl::gfx::Renderer* r) override { if(shown) ListItem::draw(r); }
    tsl::elm::Element* requestFocus(tsl::elm::Element* old,tsl::FocusDirection d) override {
        return shown?ListItem::requestFocus(old,d):nullptr;
    }
    bool onClick(u64 keys) override { return shown&&ListItem::onClick(keys); }
    bool onTouch(tsl::elm::TouchEvent e,s32 x,s32 y,s32 px,s32 py,s32 ix,s32 iy) override {
        return shown&&ListItem::onTouch(e,x,y,px,py,ix,iy);
    }
};
class ProbeGui : public tsl::Gui {
    tsl::elm::ListItem *state = nullptr, *duration = nullptr, *size = nullptr, *audio = nullptr;
    tsl::elm::ListItem *error = nullptr, *message = nullptr, *space = nullptr;
    tsl::elm::ListItem *primary = nullptr;
    StopItem* stopButton=nullptr;
    lr::Op primaryOp=lr::Op::Start;
    bool launchAttempted=false;
    Result startupResult=0;
    uint64_t lastUpdate = 0, lastCommand = 0;
    lr::Status snapshot;
    bool fresh() {
        lr::Status s;
        if (!storage.ready || !running() || !lr::readFile(overlayStatusPath, s) || !lr::valid(s)) return false;
        uint64_t now = nowUs();
        if (now < s.heartbeatUs || now - s.heartbeatUs > 4000000) return false;
        if(snapshot.boot!=s.boot) lastCommand=0;
        snapshot = s; return true;
    }
    void ensureBackend(bool retry=false) {
        if(!storage.ready||running()||(launchAttempted&&!retry)) return;
        launchAttempted=true;startupResult=launchServiceResult;
        if(R_FAILED(startupResult)) return;
        NcmProgramLocation loc{lr::ModuleId,NcmStorageId_None};u64 pid=0;
        startupResult=pmshellLaunchProgram(0,&loc,&pid);
    }
    void showStop(bool show) {
        if(!show&&stopButton->visible()&&getFocusedElement()==stopButton) {
            removeFocus();requestFocus(primary,tsl::FocusDirection::None,false);
        }
        stopButton->show(show);
    }
    void send(lr::Op op) {
        if (!fresh()) { message->setValue("正在连接录屏服务，请稍候"); return; }
        if (lastCommand && snapshot.commandId < lastCommand) {
            message->setValue("等待上一条命令确认"); return;
        }
        if(!lr::actionAllowed(snapshot.state,op)) { message->setValue("状态已变化，请按更新后的按钮操作");return; }
        lr::Command c; c.boot = snapshot.boot;
        c.id = std::max(std::max(lastCommand, snapshot.commandId) + 1, nowUs()); c.op = op;
        const bool sent=lr::replaceFile(overlayCommandPath,c);
        if(sent) lastCommand=c.id;
        message->setValue(sent ? "命令已发送，等待后台确认" : "命令写入失败，可重试");
    }
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Lite 录屏", "0.2.3 Beta · 本机 MKV 录像");
        auto* list = new tsl::elm::List();
        primary = new tsl::elm::ListItem("正在准备录屏");
        primary->setClickListener([this](u64 keys) {
            if(!(keys&HidNpadButton_A)) return false;
            if(!fresh()) { ensureBackend(true);message->setValue("正在连接录屏服务，请稍候");return true; }
            send(primaryOp);return true;
        });
        list->addItem(primary);
        stopButton = new StopItem();
        stopButton->setClickListener([this](u64 keys) { if (!(keys & HidNpadButton_A)) return false; send(lr::Op::Stop); return true; });
        list->addItem(stopButton);
        list->addItem(new tsl::elm::CategoryHeader("后台实际状态"));
        state = new tsl::elm::ListItem("状态"); list->addItem(state);
        duration = new tsl::elm::ListItem("录像时长"); list->addItem(duration);
        size = new tsl::elm::ListItem("录像总大小"); list->addItem(size);
        audio = new tsl::elm::ListItem("音频数据"); list->addItem(audio);
        space = new tsl::elm::ListItem("剩余空间"); list->addItem(space);
        error = new tsl::elm::ListItem("错误码"); list->addItem(error);
        message = new tsl::elm::ListItem("提示"); list->addItem(message);
        list->addItem(new tsl::elm::ListItem("保存在 recordings/lite-recorder"));
        frame->setContent(list);ensureBackend();return frame;
    }
    void update() override {
        uint64_t now = nowUs(); if (now - lastUpdate < 500000) return; lastUpdate = now;
        if (!storage.ready) {
            primary->setText("SD 卡未就绪");showStop(false);
            state->setValue("SD 卡未就绪");
            char b[64];
            snprintf(b, sizeof(b), "0x%X / errno %d", storage.mountResult, storage.directoryError);
            error->setValue(b);
            message->setValue(storage.mounted ? "配置目录创建失败" : "SD 卡连接失败");
            return;
        }
        if (!fresh()) {
            ensureBackend();
            primary->setText(R_FAILED(startupResult)?"重试连接":"正在准备录屏");
            primary->setValue("");showStop(false);
            state->setValue(running() ? "等待录屏服务响应" : "正在启动录屏服务");
            char b[64];snprintf(b,sizeof(b),"0x%X",startupResult);error->setValue(b);
            message->setValue(R_FAILED(startupResult)?"启动失败，检查安装后重试":"请稍候；长时间无响应请重启");
            duration->setValue("--"); size->setValue("--"); audio->setValue("--"); space->setValue("--");
            return;
        }
        const auto actions=lr::controls(snapshot.state);
        primaryOp=actions.op;
        if(primary->getText()!=actions.label) primary->setText(actions.label);
        primary->setValue(lastCommand&&snapshot.commandId<lastCommand?"处理中":"");
        showStop(actions.showStop);
        state->setValue(stateLabel(snapshot.state));
        char b[64]; const auto sec = snapshot.durationUs / 1000000;
        snprintf(b, sizeof(b), "%02llu:%02llu:%02llu", (unsigned long long)(sec / 3600),
            (unsigned long long)(sec / 60 % 60), (unsigned long long)(sec % 60));
        duration->setValue(b);
        size->setValue(sizeString(snapshot.recordedBytes));
        audio->setValue(sizeString(snapshot.audioBytes)); space->setValue(sizeString(snapshot.freeBytes));
        snprintf(b, sizeof(b), "0x%X", snapshot.result); error->setValue(b);
        if (snapshot.fault != lr::Fault::None) message->setValue(faultLabel(snapshot.fault));
        else if (snapshot.state == lr::State::NeedCodec) message->setValue("缺少视频参数，请保存进度后重开游戏");
        else if (snapshot.state == lr::State::Paused) message->setValue("已暂停，继续写入同一文件");
        else if (snapshot.state == lr::State::Waiting) message->setValue("等待可独立播放的起始画面");
        else if (snapshot.state == lr::State::Stalled) message->setValue("回到游戏等待恢复，可手动停止");
        else if (snapshot.stopReason == lr::StopReason::User) message->setValue("收到停止指令，已保存");
        else if (lastCommand && snapshot.commandId >= lastCommand) message->setValue("后台已确认");
        else message->setValue("准备就绪");
    }
};
class Overlay : public tsl::Overlay {
public:
    void initServices() override {
        // libtesla already owns pmdmnt and fs service initialization.
        launchServiceResult = pmshellInitialize();
        storage = lr::initializeOverlayStorage(
            []() { return fsdevMountSdmc(); },
            [](const char* path) { return mkdir(path, 0777) == 0 ? 0 : errno; });
    }
    void exitServices() override {
        if (storage.mounted) fsdevUnmountDevice("sdmc");
        storage = {};
        if (R_SUCCEEDED(launchServiceResult)) pmshellExit();
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<ProbeGui>(); }
};
}
int main(int argc, char** argv) { return tsl::loop<Overlay>(argc, argv); }
