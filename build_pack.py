# -*- coding: utf-8 -*-
from datetime import datetime
import subprocess
import os

now = datetime.now()
data = now.strftime("%Y%m%d")
time = now.strftime("%Y %m %d: %H:%M:%S")
ndk = "E:/ndk/android-ndk-r29-windows/android-ndk-r29/toolchains/llvm/prebuilt/windows-x86_64/bin"
clang = ndk + "/aarch64-linux-android35-clang++.cmd"
strip_cmd = ndk + "/llvm-strip.exe"
Project = os.path.dirname(os.path.abspath(__file__))
BinaryName = "LittleYouran_CTS"

def log(LogMessage):
    print(time, LogMessage)

def build():
    include_dir = Project + "/include"
    command = [clang, Project + "/src/main.cpp", "-o", BinaryName, "-static-libstdc++", "-O3", "-I" + include_dir, "-I" + Project]
    log("Compiling...")
    result = subprocess.run(command)
    if result.returncode == 0:
        log("Compile success!")
    else:
        log("Compile failed!")
        return False
    log("Stripping...")
    subprocess.run([strip_cmd, BinaryName])
    log("Done: " + BinaryName)
    return True

build()
