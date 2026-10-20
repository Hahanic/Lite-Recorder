#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <sys/stat.h>
#include "protocol.hpp"
#include "overlay_storage.hpp"

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
class ProbeGui : public tsl::Gui {
    tsl::elm::ListItem *state = nullptr, *duration = nullptr, *size = nullptr, *audio = nullptr;
    tsl::elm::ListItem *error = nullptr, *message = nullptr, *space = nullptr;
    tsl::elm::ListItem *elapsed = nullptr, *ages = nullptr;
    uint64_t lastUpdate = 0, lastCommand = 0;
    lr::Status snapshot;
    bool fresh() {
        lr::Status s;
        if (!storage.ready || !running() || !lr::readFile(overlayStatusPath, s) || !lr::valid(s)) return false;
        uint64_t now = nowUs();
        if (now < s.heartbeatUs || now - s.heartbeatUs > 4000000) return false;
        snapshot = s; return true;
    }
    void send(lr::Op op) {
        if (!fresh()) { message->setValue("请先加载后台，等状态更新"); return; }
        if (lastCommand && snapshot.commandId < lastCommand) {
            message->setValue("等待上一条命令确认"); return;
        }
        const bool active=snapshot.state==lr::State::Waiting || snapshot.state==lr::State::Recording ||
            snapshot.state==lr::State::Stalled || snapshot.state==lr::State::Paused || snapshot.state==lr::State::NeedCodec;
        if(op==lr::Op::Start&&active) { message->setValue("已有录制，请继续或停止");return; }
        if(op==lr::Op::Resume&&snapshot.state!=lr::State::Paused) { message->setValue("当前未暂停");return; }
        if(op==lr::Op::Pause&&(!active||snapshot.state==lr::State::Paused)) { message->setValue("当前无需暂停");return; }
        if(op==lr::Op::Stop&&!active) { message->setValue("当前没有进行中的录制");return; }
        lr::Command c; c.boot = snapshot.boot;
        c.id = std::max(std::max(lastCommand, snapshot.commandId) + 1, nowUs()); c.op = op;
        const bool sent=lr::replaceFile(overlayCommandPath,c);
        if(sent) lastCommand=c.id;
        message->setValue(sent ? "命令已发送，等待后台确认" : "命令写入失败，可重试");
    }
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Lite 录屏", "0.2.1 Beta · 本机 MKV 录像");
        auto* list = new tsl::elm::List();
        auto* load = new tsl::elm::ListItem("加载后台");
        load->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            if (!storage.ready) { message->setValue("SD 卡未就绪，无法加载"); return true; }
            if (R_FAILED(launchServiceResult)) {
                char b[64]; snprintf(b, sizeof(b), "启动服务错误 0x%X", launchServiceResult);
                message->setValue(b); return true;
            }
            if (running()) { message->setValue("后台已运行"); return true; }
            NcmProgramLocation loc{lr::ModuleId, NcmStorageId_None}; u64 pid = 0;
            Result rc = pmshellLaunchProgram(0, &loc, &pid);
            char b[64]; snprintf(b, sizeof(b), "加载结果 0x%X", rc); message->setValue(b);
            return true;
        });
        list->addItem(load);
        auto* start = new tsl::elm::ListItem("开始录制");
        start->setClickListener([this](u64 keys) { if (!(keys & HidNpadButton_A)) return false; send(lr::Op::Start); return true; });
        list->addItem(start);
        auto* pause = new tsl::elm::ListItem("暂停");
        pause->setClickListener([this](u64 keys) { if (!(keys & HidNpadButton_A)) return false; send(lr::Op::Pause); return true; });
        list->addItem(pause);
        auto* resume = new tsl::elm::ListItem("继续录制");
        resume->setClickListener([this](u64 keys) { if (!(keys & HidNpadButton_A)) return false; send(lr::Op::Resume); return true; });
        list->addItem(resume);
        auto* stop = new tsl::elm::ListItem("停止并保存");
        stop->setClickListener([this](u64 keys) { if (!(keys & HidNpadButton_A)) return false; send(lr::Op::Stop); return true; });
        list->addItem(stop);
        list->addItem(new tsl::elm::CategoryHeader("后台实际状态"));
        state = new tsl::elm::ListItem("状态"); list->addItem(state);
        duration = new tsl::elm::ListItem("录像累计时长"); list->addItem(duration);
        elapsed = new tsl::elm::ListItem("文件：已完成 / 总数"); list->addItem(elapsed);
        ages = new tsl::elm::ListItem("无新数据：视频 / 声音"); list->addItem(ages);
        size = new tsl::elm::ListItem("录像文件总大小"); list->addItem(size);
        audio = new tsl::elm::ListItem("音频数据"); list->addItem(audio);
        space = new tsl::elm::ListItem("剩余空间"); list->addItem(space);
        error = new tsl::elm::ListItem("错误码"); list->addItem(error);
        message = new tsl::elm::ListItem("提示"); list->addItem(message);
        list->addItem(new tsl::elm::ListItem("保存在 recordings/lite-recorder"));
        frame->setContent(list); return frame;
    }
    void update() override {
        uint64_t now = nowUs(); if (now - lastUpdate < 500000) return; lastUpdate = now;
        if (!storage.ready) {
            state->setValue("SD 卡未就绪");
            char b[64];
            snprintf(b, sizeof(b), "0x%X / errno %d", storage.mountResult, storage.directoryError);
            error->setValue(b);
            message->setValue(storage.mounted ? "配置目录创建失败" : "SD 卡连接失败");
            return;
        }
        if (!fresh()) {
            state->setValue(running() ? "后台无新状态" : "后台未加载");
            duration->setValue("--"); size->setValue("--"); audio->setValue("--"); space->setValue("--");
            elapsed->setValue("--"); ages->setValue("--");
            return;
        }
        state->setValue(stateLabel(snapshot.state));
        char b[64]; const auto sec = snapshot.durationUs / 1000000;
        snprintf(b, sizeof(b), "%02llu:%02llu:%02llu", (unsigned long long)(sec / 3600),
            (unsigned long long)(sec / 60 % 60), (unsigned long long)(sec % 60));
        duration->setValue(b);
        snprintf(b, sizeof(b), "%u / %u", snapshot.completedParts, snapshot.parts);
        elapsed->setValue(b);
        snprintf(b, sizeof(b), "%.1f / %.1f 秒", snapshot.videoAgeUs / 1e6, snapshot.audioAgeUs / 1e6);
        ages->setValue(b);
        size->setValue(sizeString(snapshot.recordedBytes));
        audio->setValue(sizeString(snapshot.audioBytes)); space->setValue(sizeString(snapshot.freeBytes));
        snprintf(b, sizeof(b), "0x%X", snapshot.result); error->setValue(b);
        if (snapshot.fault != lr::Fault::None) message->setValue(faultLabel(snapshot.fault));
        else if (snapshot.state == lr::State::NeedCodec) message->setValue("先加载后台，再重开游戏");
        else if (snapshot.state == lr::State::Paused) message->setValue("已暂停，继续写入同一文件");
        else if (snapshot.state == lr::State::Waiting) message->setValue("等待可独立播放的起始画面");
        else if (snapshot.state == lr::State::Stalled) message->setValue("回到游戏等待恢复，可手动停止");
        else if (snapshot.stopReason == lr::StopReason::User) message->setValue("收到停止指令，已保存");
        else if (lastCommand && snapshot.commandId >= lastCommand) message->setValue("后台已确认");
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
