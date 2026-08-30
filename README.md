# MPPT 数字电源控制器

本仓库维护 STM32G474RBT3 + RT-Thread Nano 的 MPPT 功率级控制固件，包含源码、Keil 工程、当前网表、检查脚本和工程日志。

## 当前版本

- 固件目录：`firmware/STM32G474RBT3_RTThread_Safe_v2.3.0/`
- 当前网表：控制板和非同步功率板两份，均位于固件目录的 `hardware/netlists/`。
- 外部硬件保护由功率板完成；固件保留软件 OVP/OCP/OTP、采样看门狗、占空比和母线建压限制。
- 上电默认所有功率输出关闭，必须先检查状态再启动。

## 接手者流程

1. 克隆：`git clone https://github.com/TaiTai-Z/MPPT.git`
2. 阅读 `docs/logs/remade_master.md` 最新记录、固件实现状态和网表审查文档。
3. 只修改固件源码、工程配置、脚本、文档和日志，不手工编辑 `Out/`。
4. 在固件目录依次运行 `tools\\check_project.bat`、`tools\\build_keil.bat`、`tools\\flash_keil.bat`。脚本使用 `D:\\Keil\\Keil\\UV4\\UV4.exe`，无窗口执行。
5. 编译必须为零错误、零警告；烧录后复位，先发 `HELP`、`STATUS`、`CONTROL`、`HRTIMDIAG`，再发 `START 150` 或 `MPPT AUTO`。

## 日志和提交

所有代码修改、检查、编译、烧录、串口和示波器实测都追加到 `docs/logs/remade_master.md`，不得覆盖历史记录。每条记录写明日期、版本、现象或需求、改动文件、证据等级、检查/编译/烧录结果、测试命令、测量结果和剩余风险。提交前运行 `git status`，不得提交生成物、密码或令牌；一个主题对应一个提交，禁止强制覆盖 `main`。

## 安全

首次功率测试必须使用限流低压源、预充、放电和急停，并使用隔离/差分探头验证 PWM、ADC 同步和保护链路。编译成功或串口回复不能证明功率级安全。
