# evdi 第三方头文件

本目录包含 libevdi 的公开头文件，用于让 daemon 在编译期获得 evdi 接口声明。
daemon 运行时链接系统已有的 libevdi（本机为 1.15）。

- 来源：USBDisplay/work/evdi-src/library/evdi_lib.h
- 上游版本：libevdi 1.15.1，上游提交 8acd0b2
- 许可证：LGPL-2.1-or-later
- 本目录仅复制头文件，未修改内容；SPDX 头保留在文件顶部。
- 系统库：/lib/libevdi.so.1；安装脚本不会替换系统库。

如果更新头文件，请保持上述许可证声明，并同步更新本说明。
