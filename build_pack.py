from datetime import datetime
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
BUILD.mkdir(exist_ok=True)


def find_ndk():
    """探测 Android NDK：环境变量优先，其次自动扫描 C/D/E 盘。"""
    for key in ("ANDROID_NDK_HOME", "ANDROID_NDK"):
        value = os.environ.get(key)
        if value:
            path = Path(value)
            if (path / "toolchains" / "llvm" / "prebuilt").exists():
                return path
    for drive in "CDE":
        base = Path(f"{drive}:\\")
        if not base.exists():
            continue
        try:
            dirs = [base] + [d for d in base.iterdir() if d.is_dir()]
        except (PermissionError, OSError):
            continue
        for directory in dirs:
            try:
                for candidate in directory.glob("android-ndk-*"):
                    if (candidate / "toolchains" / "llvm" / "prebuilt").exists():
                        return candidate
            except (PermissionError, OSError):
                continue
    return None


def main() -> None:
    ndk = find_ndk()
    if ndk is None:
        raise SystemExit(
            "找不到 Android NDK。请设置 ANDROID_NDK_HOME，或把 NDK 放到 C:/D:/E: 盘（目录名含 android-ndk）。"
        )

    prebuilt = ndk / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64"
    clang = prebuilt / "bin" / "clang++.exe"
    sysroot = prebuilt / "sysroot"
    if not clang.exists():
        raise SystemExit(f"找不到 NDK 编译器: {clang}")

    flags = [
        "--target=aarch64-linux-android27", f"--sysroot={sysroot}", "-std=c++23",
        "-static", "-s", "-O3", "-flto", "-fno-exceptions", "-ffast-math",
        "-funroll-loops", "-frtti", "-fexceptions", "-finline-functions",
        "-fomit-frame-pointer", "-Wall", "-Wextra", "-Wshadow", "-fPIE",
    ]
    command = [str(clang), *flags, "-I", str(ROOT / "include"),
               str(ROOT / "src" / "main.cpp"), "-o", str(BUILD / "Littleyouran")]
    print(f"使用 NDK: {ndk}")
    subprocess.run(command, check=True)
    print(f"{datetime.now():%Y-%m-%d %H:%M:%S} Release 版本编译成功 -> {BUILD / 'Littleyouran'}")


if __name__ == "__main__":
    main()
