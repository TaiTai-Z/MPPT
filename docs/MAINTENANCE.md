# 工程维护与接手规范

## 真值源

唯一维护固件是 `firmware/STM32G474RBT3_RTThread_Safe_v2.3.0/`。硬件判断只依据其中的控制板和非同步功率板网表。允许修改源码、Keil 工程、脚本、说明和日志；禁止修改或提交 `Out/` 生成文件，禁止删除历史日志，禁止把未实测功能写成已完成。

## 每次修改

1. 先读 `docs/logs/remade_master.md`。
2. 记录现象、需求、复现命令和安全边界。
3. 检查 GPIO 复用、ADC、HRTIM PWM/FLT/EEV、DMA、触发源、时钟和链接内存。
4. 运行 `tools\\check_project.bat` 和 `tools\\build_keil.bat`，要求零错误、零警告。
5. 只有用户明确允许且构建通过后才运行 `tools\\flash_keil.bat`。
6. 烧录后复位，先查询 `STATUS`、`CONTROL`、`HRTIMDIAG`。
7. 将全过程追加到统一日志，原始输出放入 `docs/build-logs/`。

## 日志格式

每条记录必须包含日期/版本、现象或需求、根因和证据、修改文件及行为、静态检查、编译、烧录（如有）、串口或示波器结果、剩余风险和下一步。证据等级使用 `[STATIC]`、`[ARMCLANG]`、`[FLASH]`、`[TARGET]`；编译不能代替实板验证。

## 安全与 Git

高压母线测试必须限流、预充、放电和急停；软件保护不能替代外部硬件保护。提交前检查 `git status`，一个主题一个提交，核对远端后再推送，禁止强制覆盖 `main`。
