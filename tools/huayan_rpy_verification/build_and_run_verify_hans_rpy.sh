#!/usr/bin/env bash

# 编译并运行华沿机器人欧拉角顺序验证工具。
#
# 使用方式：
#   ./tools/huayan_rpy_verification/build_and_run_verify_hans_rpy.sh <机器人控制器IP>
#
# 示例：
#   ./tools/huayan_rpy_verification/build_and_run_verify_hans_rpy.sh 192.168.1.11
#
# 安全说明：
#   被编译的 C++ 程序只调用连接、RPY 转四元数和断开连接接口，
#   不包含上电、使能或机器人运动指令。

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "用法：$0 <机器人控制器IP>" >&2
    exit 1
fi

# 根据脚本自身位置定位项目根目录，避免依赖执行脚本时的当前工作目录。
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_directory}/../.." && pwd)"
robot_ip="$1"

sdk_root="${project_root}/3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0"
sdk_header="${sdk_root}/include/HR_Pro.h"
sdk_library="${sdk_root}/Linux/libHR_Pro.so"
source_file="${script_directory}/verify_hans_rpy.cpp"
output_file="${script_directory}/verify_hans_rpy"

if ! command -v g++ >/dev/null 2>&1; then
    echo "错误：未找到 g++，请先安装支持 C++17 的编译器。" >&2
    exit 2
fi

if [[ ! -f "${source_file}" ]]; then
    echo "错误：找不到测试源文件：${source_file}" >&2
    exit 3
fi

if [[ ! -f "${sdk_header}" ]]; then
    echo "错误：找不到华沿 SDK 头文件：${sdk_header}" >&2
    exit 4
fi

if [[ ! -f "${sdk_library}" ]]; then
    echo "错误：找不到华沿 SDK 动态库：${sdk_library}" >&2
    exit 5
fi

echo "正在编译欧拉角顺序验证工具……"

# $ORIGIN 在运行时表示可执行文件所在目录。这里保留字面量，
# 使程序能够从项目内固定的 SDK Linux 目录加载 libHR_Pro.so。
g++ \
    -std=c++17 \
    -Wall \
    -Wextra \
    "${source_file}" \
    -I"${sdk_root}/include" \
    -L"${sdk_root}/Linux" \
    -lHR_Pro \
    '-Wl,-rpath,$ORIGIN/../../3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0/Linux' \
    -o "${output_file}"

echo "编译完成，正在连接机器人控制器：${robot_ip}"
echo

"${output_file}" "${robot_ip}"
