## F9 分支修改概要

本分支基于 avdvplus 主线，针对 Amlogic 平台进行了多项增强和修复，主要改动如下：

### 1. HDR 稳定性修复 （修复爱奇艺片源卡死）
- **SEI NAL 防护**：对 SEI NAL 进行 payload 数据类型白名单校验（MDCV=137 / CLL=144 / HDR10+=4），防止损坏数据触发 `std::bad_alloc` 导致崩溃
- **DoVi RPU 空指针修复**：`PopulateDoviRpuInfo` 增加 null header 保护，防止特定流崩溃
- 修复大部分无法播放的bug片源

### 2. ISO 蓝光播放
- **HTTP ISO 流引擎**：实现基于 HTTP Range 的 ISO 302远程播放，支持 CDN 重定向缓存
- **页面级 LRU 缓存**：蓝光 ISO 播放引入页面级 LRU 缓存，优化随机读取性能
- **线程安全修复**：`m_isoCache` 使用 `shared_ptr` + 互斥锁，消除 TOCTOU use-after-free 风险

### 3. 字幕显示优化
- **动态字幕偏移**：支持 UP/DOWN 按键实时调整字幕位置，默认启用本地字幕移动
- **PGS 字幕位置修正**：`ManageRenderArea()` 移入新帧保护，消除位置抖动
- **垂直移动步长优化**：从 1% 降至 0.5%，提供更精细的控制
- **Flush 时重置视图高度**：切换视频时强制重新计算字幕位置

### 4. HDR Vivid 支持
- **支持 HDR Vivid 元数据转为 DV RPU
- **支持 HDR Vivid DV Hybrid 优先级选择
- **支持 HDR Vivid 原生输出到支持的显示设备

### 5. CPU 温度
- **CPU 温度监控**：绝大部分 905x2 905x4 922x 支持FEL解码的芯片都能显示温度



鸣谢：CoreELEC团队所有开发者  CPM  AVDVPLUS  GLSIMON  晓

赞助Donate:

- [爱发电](https://afdian.com/a/mephis)

- [paypal](https://www.paypal.com/ncp/payment/BX82KFCMVUT34)
