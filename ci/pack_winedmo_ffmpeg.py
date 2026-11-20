#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 FFmpeg 8.1.1 及其依赖闭包随 Wine 一起分发，让 winedmo.so 能真正加载。

背景
----
`dlls/winedmo/Makefile.in` 里只有：

    UNIX_CFLAGS = $(FFMPEG_CFLAGS)
    UNIX_LIBS   = $(FFMPEG_LIBS) $(PTHREAD_LIBS)

没有独立的 --enable-winedmo 开关。`configure --with-ffmpeg` 成功后，
`FFMPEG_LIBS` 展开为 `-lavformat -lavcodec -lavutil ...`，winedmo.so 的
DT_NEEDED 里就会出现 libavformat.so.62 等；而 Wine 的 unix 库是运行时
dlopen 的，目标设备（Termux/Android，glibc+box64）上没有 /tmp/ffmpeg/lib，
apt 装的 libdav1d / libgnutls / liblzma 也不会被带过去。

本脚本做五件事：
  1. 以 `$FFMPEG_PREFIX/lib` 下的 libav* 为根，做 DT_NEEDED 依赖闭包 BFS
     （跨多个搜索目录：ffmpeg 自己的 lib、apt 的 multiarch 目录等），
     把 libav*、libdav1d、libgnutls(+nettle/hogweed/gmp/unistring/idn2/
     tasn1/p11-kit/ffi)、libbz2、liblzma、libz、libzstd 全部收齐。
     白名单（AV/DEP_PREFIXES）仍是最终闸门，glibc / libstdc++ / libgcc_s /
     ld-linux 一律不带。
  2. 只拷进 ELFCLASS64 的 `*-unix` 目录。实测本仓库产物形态是：
       proton  -> lib/wine/{x86_64-unix, i386-windows, x86_64-windows}
       wine.yml-> lib/wine/{aarch64-unix, i386-windows, aarch64-windows}
     即 unix 侧只有一个 host 架构目录（`--enable-archs=i386,x86_64` 只影响
     PE 侧，不产生 i386-unix）。这里仍然按 ELF class 过滤，是为了防止将来
     引入真正的双架构 unix 构建时把 64 位库塞进 32 位目录而毁掉 syswow64。
  3. 拷贝按 SONAME 落名（实体文件），其余别名建成指向 SONAME 的相对符号
     链接，保证 DT_NEEDED 里的 `libavformat.so.62` 能命中。
  4. 给这批库以及 winedmo.so 在既有 rpath 前面插入 $ORIGIN（保留原有条目，
     例如 Termux 的 /data/data/com.termux/files/usr/glibc/lib），使 libav*
     优先从自身目录解析（先 strip 再 patchelf，否则 strip 会丢掉 rpath）。
  5. 校验 winedmo.so 的 DT_NEEDED 含 libav*、体积正常、
     `ldd`（LD_LIBRARY_PATH=目标目录）无 not found。

用法
----
    python3 pack_winedmo_ffmpeg.py <wine-prefix> <ffmpeg-prefix>
"""

import os
import re
import shutil
import subprocess
import sys

# 种子根：从 winedmo.so 真正 DT_NEEDED 的库出发，避免把空壳的
# libavdevice / libavfilter 也打进包里（BFS 会沿 DT_NEEDED 自动补齐
# libswresample 等真正被依赖的库）。
# libswscale 必须显式做种子：winedmo.so 直接 NEEDED 它，但
# libavcodec/libavformat 并不依赖 swscale，光靠 BFS 永远收不到，
# 校验阶段会报 libswscale.so.9 => not found。
SEED_PREFIXES = ("libavutil", "libavformat", "libavcodec", "libswscale")

# 第一梯队：FFmpeg 本体
AV_PREFIXES = (
    "libavutil",
    "libavcodec",
    "libavformat",
    "libavdevice",
    "libavfilter",
    "libpostproc",
    "libswresample",
    "libswscale",
)

# 第二梯队：GE 的 FFMPEG_DEPENDS = bzip2 dav1d xz，加上 gnutls 及其传递依赖。
# 这些在 Termux/Android 上通常不存在，必须自带。
DEP_PREFIXES = (
    "libdav1d",
    "libgnutls",
    "libnettle",
    "libhogweed",
    "libgmp",
    "libunistring",
    "libidn2",
    "libtasn1",
    "libp11-kit",
    "libffi",
    "libbz2",
    "liblzma",
    "libz.so",
    "libzstd",
)

# 第三梯队：编码器库。仓库自己编的 FFmpeg 是精简配置（--disable-everything），
# 而 glibc-packages 的预编译包是全功能版，libavcodec.so.62 的 DT_NEEDED
# 里直接就有 libvpx/libx264/libx265/libxvidcore/libtheora/libwebp/libwebpmux。
# 不放进白名单会被 BFS 无声跳过，ldd 校验阶段才报 not found。
CODEC_PREFIXES = (
    "libvpx",
    "libx264",
    "libx265",
    "libxvidcore",
    "libtheora",      # 同时覆盖 libtheoraenc / libtheoradec
    "libwebp",        # 同时覆盖 libwebpmux / libwebpdemux
    "libsharpyuv",    # libwebp 的 DT_NEEDED
    "libogg",         # libtheora 的 DT_NEEDED
)

# 绝对不能带走的系统基础库（glibc / libstdc++，设备侧有自己的）
SKIP_PREFIXES = (
    "libc.so",
    "libm.so",
    "libdl.so",
    "libpthread.so",
    "librt.so",
    "libresolv.so",
    "libutil.so",
    "ld-linux",
    "libgcc_s",
    "libstdc++",
    "libatomic",
    "libgomp",
    "libmvec",      # glibc 的 SIMD 数学库，libx265 需要它；必须用设备自带版本
)

# 依赖闭包 BFS 的搜索目录（按优先级）。除了 FFmpeg 自己的 lib，还要能
# 找到 apt 装的 libdav1d/libgnutls/libbz2/liblzma/libz/libzstd/libffi。
EXTRA_SEARCH_DIRS = (
    "/usr/lib/x86_64-linux-gnu",
    "/lib/x86_64-linux-gnu",
    "/usr/lib64",
    "/usr/lib",
    "/lib64",
    "/lib",
    "/usr/local/lib",
)

ELFCLASS64 = 2


def log(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    # 强制 C locale：CI 里 LANG=zh_CN.UTF-8 时 readelf/objdump 会输出中文
    # （"共享库：[libz.so.1]"），按英文正则解析会得到 0 条 DT_NEEDED。
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    env["LANG"] = "C"
    kw.setdefault("env", env)
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)


def read_elf_class(path):
    """返回 EI_CLASS（1=32 位, 2=64 位），非 ELF 或读失败返回 None。"""
    try:
        with open(path, "rb") as fh:
            head = fh.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    return head[4]


def read_soname(path):
    r = run(["readelf", "-d", path])
    if r.returncode != 0:
        return None
    m = re.search(r"SONAME\)\s+Library soname:\s+\[([^\]]+)\]", r.stdout)
    return m.group(1) if m else None


def read_dyn(path):
    """返回 readelf -d 的原始输出（失败时返回空串并记录原因）。"""
    r = run(["readelf", "-d", path])
    return r.returncode, (r.stdout or "")


NEEDED_RE = re.compile(r"NEEDED\)\s+Shared library:\s+\[([^\]]+)\]")
NEEDED_LOOSE_RE = re.compile(r"\(NEEDED\)[^\n]*\[([^\]]+)\]")


def read_needed(path):
    """解析 DT_NEEDED；readelf 解析为空时强制回退 objdump -p。"""
    rc, out = read_dyn(path)
    if rc == 0:
        libs = NEEDED_RE.findall(out)
        if not libs:
            # 非英文 locale 或格式差异时用宽松正则兜底
            libs = NEEDED_LOOSE_RE.findall(out)
        if libs:
            return libs
    r = run(["objdump", "-p", path])
    return re.findall(r"NEEDED\s+(\S+)", r.stdout or "")


def diagnose(path):
    """打印 ELF 诊断信息，用于 readelf 读不到内容时定位原因。"""
    log("    --- 诊断 %s ---" % path)
    try:
        log("      size=%d" % os.path.getsize(path))
    except OSError as exc:
        log("      stat 失败: %s" % exc)
    for cmd in (["bash", "-lc", "file '%s' 2>&1 || true" % path],
                ["readelf", "-h", path],
                ["readelf", "-S", path]):
        r = run(cmd)
        for line in (r.stdout or "").splitlines():
            log("      " + line[:200])
    rc, out = read_dyn(path)
    log("      readelf -d rc=%d，输出 %d 行" % (rc, len(out.splitlines())))
    for line in out.splitlines()[:30]:
        log("      " + line[:200])
    r = run(["objdump", "-p", path])
    log("      objdump -p rc=%d" % r.returncode)
    for line in (r.stdout or "").splitlines()[:30]:
        log("      " + line[:200])
    # 找构建树里的原始 winedmo.so + config.h 的 HAVE_FFMPEG
    ws = os.environ.get("GITHUB_WORKSPACE", "")
    if ws:
        r = run(["bash", "-lc",
                 "find '%s' /tmp/build_wine -name config.h -path '*include*' "
                 "2>/dev/null | head -3 | xargs -r grep -H 'HAVE_FFMPEG' || true"
                 % ws])
        for line in (r.stdout or "").splitlines():
            log("      config.h: " + line[:200])
        r = run(["bash", "-lc",
                 "find /tmp/build_wine -path '*dlls/winedmo/winedmo.so' "
                 "2>/dev/null | head -2"])
        for line in (r.stdout or "").splitlines():
            if not line.strip():
                continue
            log("      构建树原始文件: " + line)
            rc2, out2 = read_dyn(line.strip())
            log("        rc=%d 行数=%d" % (rc2, len(out2.splitlines())))
            for ln in out2.splitlines()[:20]:
                log("        " + ln[:200])


def is_seed(name):
    return any(name.startswith(p) for p in SEED_PREFIXES)


def wanted(name):
    return (any(name.startswith(p) for p in AV_PREFIXES)
            or any(name.startswith(p) for p in DEP_PREFIXES)
            or any(name.startswith(p) for p in CODEC_PREFIXES))


def skipped(name):
    return any(name.startswith(p) for p in SKIP_PREFIXES)


def resolve(soname, search_dirs):
    """按 ld.so 的搜索顺序找 soname，返回真实路径或 None。"""
    for d in search_dirs:
        p = os.path.join(d, soname)
        if os.path.exists(p):
            return os.path.realpath(p)
    return None


def collect(src_libdir, search_prefixes):
    """从 src_libdir 的 libav* 出发做 DT_NEEDED 闭包。

    返回 (groups, aliases)：
      groups  = {soname: realpath}
      aliases = {soname: [该 soname 在源目录里出现过的其它链接名]}
    """
    groups, aliases = {}, {}
    search_dirs = [src_libdir] + list(search_prefixes)

    # 种子：src_libdir 下 libavutil/libavformat/libavcodec 的 64 位实体
    queue = []
    for name in sorted(os.listdir(src_libdir)):
        if skipped(name) or not is_seed(name):
            continue
        full = os.path.join(src_libdir, name)
        if not os.path.exists(full):
            continue
        real = os.path.realpath(full)
        if read_elf_class(real) != ELFCLASS64:
            continue
        soname = read_soname(real) or os.path.basename(real)
        if soname not in groups:
            groups[soname] = real
            queue.append(soname)
        if name != soname:
            aliases.setdefault(soname, []).append(name)

    # BFS：沿 DT_NEEDED 往下找，只保留白名单内、且是 64 位的库
    guard = 0
    while queue:
        guard += 1
        if guard > 500:
            log("  WARN: 依赖闭包层数异常，提前中止")
            break
        soname = queue.pop(0)
        real = groups[soname]
        for dep in read_needed(real):
            if dep in groups:
                continue
            if skipped(dep) or not wanted(dep):
                continue
            p = resolve(dep, search_dirs)
            if p is None:
                log("  WARN: 找不到依赖 %s（%s 需要）"
                    % (dep, os.path.basename(real)))
                continue
            if read_elf_class(p) != ELFCLASS64:
                continue
            groups[dep] = p
            queue.append(dep)

    # 补齐别名：扫描所有搜索目录里指向同一实体的链接名
    for d in search_dirs:
        if not os.path.isdir(d):
            continue
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for name in names:
            if skipped(name) or not wanted(name):
                continue
            full = os.path.join(d, name)
            if not os.path.exists(full):
                continue
            real = os.path.realpath(full)
            for soname, path in groups.items():
                if path == real and name != soname:
                    lst = aliases.setdefault(soname, [])
                    if name not in lst:
                        lst.append(name)
                    break

    return groups, aliases


def pick_target_dirs(libwine):
    """只挑 64 位的 *-unix 目录。"""
    out = []
    if not os.path.isdir(libwine):
        return out
    for name in sorted(os.listdir(libwine)):
        d = os.path.join(libwine, name)
        if not (os.path.isdir(d) and name.endswith("-unix")):
            continue
        cls = None
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if os.path.isfile(p) and not os.path.islink(p):
                cls = read_elf_class(p)
                if cls is not None:
                    break
        if cls == ELFCLASS64:
            out.append(d)
        else:
            log("  跳过 %s（非 64 位 unix 目录）" % name)
    return out


def ensure_patchelf():
    if shutil.which("patchelf"):
        return True
    log("WARN: 未找到 patchelf，尝试 apt 安装")
    run(["bash", "-lc",
         "sudo apt-get install -y -qq patchelf >/dev/null 2>&1 || true"])
    return bool(shutil.which("patchelf"))


def get_rpath(path):
    r = run(["patchelf", "--print-rpath", path])
    if r.returncode != 0:
        return ""
    return (r.stdout or "").strip()


def set_rpath_keep(path):
    """把 $ORIGIN 前置到已有 rpath 前面，保留原有条目。

    重要：不能直接 `--set-rpath "$ORIGIN"` 覆盖。本仓库的 Wine unix 库已带
    RUNPATH（如 /data/data/com.termux/files/usr/glibc/lib），覆盖掉会导致设备
    侧 libc 解析路径丢失。patchelf 默认写 DT_RUNPATH（不是 DT_RPATH），而
    DT_RUNPATH 的优先级低于 LD_LIBRARY_PATH，因此保留原值是安全做法。
    """
    old = get_rpath(path)
    parts = [p for p in old.split(":") if p]
    if "$ORIGIN" in parts:
        new = old
    else:
        new = "$ORIGIN" + (":" + old if old else "")
    r = run(["patchelf", "--set-rpath", new, path])
    if r.returncode != 0:
        log("  WARN: patchelf 失败 %s: %s"
            % (os.path.basename(path), (r.stdout or "").strip()[:200]))
        return False
    return True


def strip_then_rpath(path):
    """先 strip 再设 RPATH：strip 会重写段表，导致之前设的 RPATH 丢失。"""
    for tool in ("llvm-strip", "strip"):
        if shutil.which(tool):
            run([tool, "--strip-unneeded", path])
            break
    return set_rpath_keep(path)


def max_glibc_ver(path):
    """扫描 .dynstr 里出现的 GLIBC_2.xx 版本符号，返回最高者。

    随包分发的是 runner（Ubuntu 24.04, glibc 2.39）上链接出来的库，目标设备
    是 Termux glibc。如果设备 glibc 更旧，运行期会报
    `version 'GLIBC_2.38' not found`。这里只做报告，不作为失败条件。
    """
    r = run(["bash", "-lc",
             "strings -a %s 2>/dev/null | grep -oE '^GLIBC_[0-9]+\\.[0-9]+$' "
             "| sort -uV | tail -1" % path])
    return (r.stdout or "").strip()


def main():
    if len(sys.argv) < 3:
        log("用法: pack_winedmo_ffmpeg.py <wine-prefix> <ffmpeg-prefix>")
        return 2

    prefix = os.path.abspath(sys.argv[1])
    ffprefix = os.path.abspath(sys.argv[2])
    src_lib = os.path.join(ffprefix, "lib")

    if not os.path.isdir(prefix):
        log("FATAL: Wine 前缀不存在: %s" % prefix)
        return 1
    if not os.path.isdir(src_lib):
        log("FATAL: FFmpeg lib 目录不存在: %s" % src_lib)
        return 1

    groups, aliases = collect(src_lib, EXTRA_SEARCH_DIRS)
    if not groups:
        log("FATAL: 在 %s 里没找到任何 64 位 libav* 共享库" % src_lib)
        return 1

    targets = pick_target_dirs(os.path.join(prefix, "lib", "wine"))
    if not targets:
        log("FATAL: %s/lib/wine 下没有 64 位的 *-unix 目录" % prefix)
        return 1

    # dry-run：只校验依赖闭包与 ABI，不复制库、不改 rpath。
    # 用于「设备侧已自带 libav*」的构建，避免把 FFmpeg 重复打进产物。
    dry = os.environ.get("WINEDMO_DRY_RUN", "").lower() in ("1", "true", "yes")
    if dry:
        log("=" * 62)
        log("DRY-RUN：只校验，不分发（WINEDMO_DRY_RUN=1）")
        log("  源      : %s" % src_lib)
        log("  闭包    : %d 个 soname" % len(groups))
        for soname in sorted(groups):
            log("    %-26s <- %s" % (soname, groups[soname]))
        log("=" * 62)
        ok = True
        for d in targets:
            wd = os.path.join(d, "winedmo.so")
            if not os.path.isfile(wd):
                log("  WARN: %s 里没有 winedmo.so" % os.path.basename(d))
                continue
            needed = read_needed(wd)
            av = [n for n in needed if n.startswith("libav")]
            log("  %s/winedmo.so: %d 条 DT_NEEDED，大小 %d bytes"
                % (os.path.basename(d), len(needed), os.path.getsize(wd)))
            log("    含 libav*: %s" % (" ".join(av) if av else "(无)"))
            if not av:
                log("  WARN: winedmo.so 未链接任何 libav* —— "
                    "configure 时 FFMPEG_LIBS 为空，winedmo 是空壳")
                ok = False
        if ok:
            log("OK: winedmo 依赖校验通过（未分发任何库）")
        else:
            log("WARN: winedmo 依赖校验未通过，但 dry-run 不阻断构建")
        return 0

    log("=" * 62)
    log("分发 FFmpeg 共享库 -> Wine unix 库目录")
    log("  源      : %s" % src_lib)
    log("  实体库  : %d 个" % len(groups))
    for soname in sorted(groups):
        log("    %-26s <- %s" % (soname, groups[soname]))
    log("  目标    : %s" % ", ".join(os.path.basename(d) for d in targets))
    log("=" * 62)

    # 1) 实体文件 + 别名符号链接
    for d in targets:
        copied = linked = 0
        for soname, real in sorted(groups.items()):
            dst = os.path.join(d, soname)
            for stale in [dst] + [os.path.join(d, a)
                                  for a in aliases.get(soname, [])]:
                if os.path.lexists(stale):
                    try:
                        os.unlink(stale)
                    except OSError:
                        pass
            try:
                shutil.copy2(real, dst + ".tmp")
                os.replace(dst + ".tmp", dst)
                os.chmod(dst, 0o755)
                copied += 1
            except OSError as exc:
                log("  WARN: 拷贝 %s 失败: %s" % (soname, exc))
                continue
            for alias in aliases.get(soname, []):
                try:
                    os.symlink(soname, os.path.join(d, alias))
                    linked += 1
                except OSError as exc:
                    log("  WARN: 建链 %s -> %s 失败: %s" % (alias, soname, exc))
        log("  %s: 实体 %d 个，别名链接 %d 个"
            % (os.path.basename(d), copied, linked))

    # 2) RPATH=$ORIGIN
    have_patchelf = ensure_patchelf()
    if have_patchelf:
        patched = failed = 0
        for d in targets:
            for soname in groups:
                p = os.path.join(d, soname)
                if not os.path.isfile(p):
                    continue
                if strip_then_rpath(p):
                    patched += 1
                else:
                    failed += 1
        log("  库 RPATH=$ORIGIN: 成功 %d，失败 %d" % (patched, failed))

        # winedmo.so 自己也要 $ORIGIN，否则 dlopen 它之后找不到同目录的 libav*。
        # （Wine 的 unix 库链接时不带 rpath：UNIXLDFLAGS 只有
        #   -shared -Wl,-Bsymbolic -Wl,-soname；以前这些库只依赖 libc，
        #   不需要 rpath，winedmo 是第一个例外。）
        for d in targets:
            wd = os.path.join(d, "winedmo.so")
            if not os.path.isfile(wd):
                continue
            before = read_needed(wd)
            log("  改前 %s/winedmo.so: %d 条 DT_NEEDED %s"
                % (os.path.basename(d), len(before),
                   (" ".join(before))[:160]))
            if set_rpath_keep(wd):
                after = read_needed(wd)
                log("  改后 %s/winedmo.so: %d 条 DT_NEEDED %s"
                    % (os.path.basename(d), len(after),
                       (" ".join(after))[:160]))
            else:
                log("  WARN: winedmo.so 设 rpath 失败 (%s)"
                    % os.path.basename(d))
    else:
        log("  WARN: 无 patchelf，跳过 RPATH")

    # 3) 校验
    ok = True
    for d in targets:
        wd = os.path.join(d, "winedmo.so")
        if not os.path.isfile(wd):
            log("  WARN: %s 里没有 winedmo.so" % os.path.basename(d))
            continue
        needed = read_needed(wd)
        av = [n for n in needed if n.startswith("libav")]
        size = os.path.getsize(wd)
        log("  %s/winedmo.so: %d 条 DT_NEEDED，大小 %d bytes"
            % (os.path.basename(d), len(needed), size))
        log("    含 libav*: %s" % (" ".join(av) if av else "(无)"))
        if not needed:
            # readelf 都没解出任何 DT_NEEDED，先打印诊断再判定
            diagnose(wd)
        if size < 4096:
            log("  FATAL: winedmo.so 体积异常小，疑似空壳")
            ok = False
        if not av:
            log("  FATAL: winedmo.so 未链接任何 libav* —— "
                "configure 时 FFMPEG_LIBS 为空，winedmo 是空壳")
            ok = False
            continue

        # 检查 rpath 是否含 $ORIGIN
        rd = run(["bash", "-lc",
                  "patchelf --print-rpath %s 2>/dev/null || true" % wd])
        rp = (rd.stdout or "").strip()
        if "$ORIGIN" in rp:
            log("    rpath: %s (含 $ORIGIN OK)" % rp)
        else:
            log("    WARN: winedmo.so rpath=%s（缺少 $ORIGIN）" % (rp or "<none>"))

        # 3a) 静态 DT_NEEDED 闭包校验（不依赖构建机能否加载库）。
        # 预编译包是 glibc 2.44 编的，runner 只有 2.39，ldd 必然报
        # "version `GLIBC_2.44' not found"；那是构建机 glibc 太旧，
        # 不是打包缺失，所以真正判定交给 readelf -d。
        missing = []
        seen = set([wd])
        queue = [wd]
        while queue:
            cur = queue.pop()
            for dep in read_needed(cur):
                if dep in seen:
                    continue
                seen.add(dep)
                if skipped(dep) or not wanted(dep):
                    continue
                p = os.path.join(d, dep)
                if os.path.lexists(p):
                    queue.append(p)
                else:
                    missing.append("%s -> %s" % (os.path.basename(cur), dep))
        if missing:
            log("  FATAL: DT_NEEDED 闭包缺库:")
            for m in missing:
                log("    " + m)
            ok = False
        else:
            log("    DT_NEEDED 闭包: 全部命中（%d 个 soname）" % len(seen))

        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = d
        env["LC_ALL"] = "C"
        env["LANG"] = "C"
        r = run(["bash", "-lc",
                 "ldd %s 2>&1 | grep -E 'not found' | grep -v 'version ' || true"
                 % wd], env=env)
        miss = (r.stdout or "").strip()
        if miss:
            log("  FATAL: winedmo.so 仍有未解析依赖:")
            log("    " + miss.replace("\n", "\n    "))
            ok = False
        else:
            log("    ldd: 无缺失依赖（构建机 glibc 版本差异不计）")

        # glibc 符号版本报告（不作失败条件）
        gmax = max_glibc_ver(wd)
        if gmax:
            log("    winedmo.so 需 glibc >= %s" % gmax.replace("GLIBC_", ""))
        worst = ""
        worst_lib = ""
        for soname in sorted(groups):
            p = os.path.join(d, soname)
            if not os.path.isfile(p):
                continue
            g = max_glibc_ver(p)
            if g and (not worst or
                      [int(x) for x in g.replace("GLIBC_", "").split(".")]
                      > [int(x) for x in worst.replace("GLIBC_", "").split(".")]):
                worst, worst_lib = g, soname
        if worst:
            log("    随包库最高 glibc 需求: %s（%s）"
                % (worst.replace("GLIBC_", ""), worst_lib))
            log("    提示: 若设备 Termux glibc 更旧，运行期会报 "
                "version '%s' not found" % worst)

    log("=" * 62)
    if ok:
        log("OK: winedmo + FFmpeg 打包完成")
    else:
        log("FAIL: winedmo 打包校验未通过（见上）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
