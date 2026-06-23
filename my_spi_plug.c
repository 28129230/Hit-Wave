#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 1. 插件私有数据结构
typedef struct {
    snd_pcm_extplug_t ext;      // 必须作为第一个成员
    // 阶段2会在这里扩展：ring_buffer, spi_fd, gpio_fd, pthread_t 等
} my_spi_plug_t;

// 2. transfer 回调：零阻塞，立即返回
static snd_pcm_sframes_t my_transfer(snd_pcm_extplug_t *ext,
                                     const snd_pcm_channel_area_t *dst_areas,
                                     snd_pcm_uframes_t dst_offset,
                                     const snd_pcm_channel_area_t *src_areas,
                                     snd_pcm_uframes_t src_offset,
                                     snd_pcm_uframes_t size) {
    // 轻量级计数器：每累计 1024 帧打印一次，证明 hw_ptr 在前进
    static unsigned long total_frames = 0;
    total_frames += size;
    if (total_frames % 1024 < size) {
        fprintf(stderr, "[PLUGIN] transfer: %lu frames (hw_ptr advancing)\n", total_frames);
    }
    
    // ⭐ 黄金法则：绝不阻塞，不拷贝数据，不操作硬件
    return size;  // 返回帧数，ALSA 自动更新内部 hw_ptr
}

// 3. close 回调：释放资源
static int my_close(snd_pcm_extplug_t *ext) {
    my_spi_plug_t *plug = (my_spi_plug_t *)ext;
    fprintf(stderr, "[PLUGIN] close called, resources freed.\n");
    free(plug);
    return 0;
}

// 4. 定义回调函数集合
static const snd_pcm_extplug_callback_t my_callback = {
    .transfer = my_transfer,
    .close    = my_close,
    // .init     = NULL,   // 阶段1用不到
    // .start    = NULL,   // 由外部插件框架自动处理
    // .stop     = NULL,
};

// 5. 插件入口函数
SND_PCM_PLUGIN_DEFINE_FUNC(my_spi_plugin) {
    my_spi_plug_t *plug;
    int err;

    // 分配内存
    plug = calloc(1, sizeof(*plug));
    if (!plug)
        return -ENOMEM;

    // ======== 核心修改：采用你的手动初始化方式 ========
    memset(&plug->ext, 0, sizeof(plug->ext));          // 彻底清零
    plug->ext.version = SND_PCM_EXTPLUG_VERSION;       // 设置版本
    plug->ext.name = "My SPI Audio Sink (Phase 1)";    // 显示名称
    plug->ext.callback = &my_callback;                 // 挂载回调
    plug->ext.channels = 2;                            // 强制立体声
    plug->ext.rate = 48000;                            // 强制 48kHz（参数协商硬约束）
    plug->ext.formats = SND_PCM_FORMAT_S16_LE;         // 强制 16位
    // ================================================

    // 创建 PCM 句柄（这一步将插件注册到 ALSA 核心层）
    err = snd_pcm_extplug_create(&plug->ext, name, root, conf, stream, mode);
    if (err < 0) {
        free(plug);
        return err;
    }

    // 将 PCM 句柄返回给调用者
    *pcmp = plug->ext.pcm;
    fprintf(stderr, "[PLUGIN] Init successful, device '%s' ready.\n", name);
    return 0;
}

// 6. 导出符号（必须）
SND_PCM_PLUGIN_SYMBOL(my_spi_plugin);
