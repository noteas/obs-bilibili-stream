#!/usr/bin/env python3
"""Qt 符号审计:插件 .so/.dll 引用的 Qt 符号必须 ⊆ tests/qt-symbol-allowlist.txt。

为什么要有它(ADR-0010):侧栏让插件第一次链接 Qt,而插件的运行期 Qt 永远是「用户那台
OBS 自带的那份」,通常比构建期的 Qt 旧(OBS 30/31 自带 Qt 6.5~6.8,CI 用 6.9.1)。为此插件
以 QT_NO_VERSION_TAGGING 关掉了 Qt 的版本门禁(否则旧 Qt 直接 dlopen 失败),代价是
向下兼容改由「只用最老受支持 Qt 也导出的符号」来保证:allowlist 里的每个符号都人工对照
过 Qt 6.2.4(Ubuntu 22.04 的 OBS 30 运行时)的导出表。任何新符号 ⇒ CI 失败 ⇒ 先核对
它存在于目标下限 Qt 再加进 allowlist。

键的粒度 = 「类::成员」(不区分重载/签名),这样 Itanium(GCC/Clang)与 MSVC 两套修饰名
可以共用一份名单。用法:
    check_qt_symbols.py <插件二进制> [allowlist 文件]
"""
import os
import re
import subprocess
import sys


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, errors="replace").stdout


def base(name):
    """去掉 ELF 符号版本后缀(name@@Qt_6 → name)。"""
    return name.split("@", 1)[0]


def key_demangled(dem):
    """Itanium 修饰名 demangle 后 → 类::成员 / 自由函数名 / 类::vtable。"""
    for prefix in ("non-virtual thunk to ", "virtual thunk to ", "guard variable for ",
                   "typeinfo name for ", "typeinfo for "):
        if dem.startswith(prefix):
            dem = dem[len(prefix):]
    if dem.startswith("vtable for "):
        return dem[len("vtable for "):].strip() + "::vtable"
    m = re.match(r"^([A-Za-z_][\w:<>]*)::(~?\w+|operator[^ (]*)", dem)
    if m:
        return m.group(1) + "::" + m.group(2)
    m = re.match(r"^([A-Za-z_]\w*)", dem)
    return m.group(1) if m else dem.strip()


def key_msvc(decorated):
    """MSVC 修饰名 → 类::成员(构造/析构/虚表单独归一;支持嵌套类 Outer@Inner)。"""
    # ??0Ctor / ??1Dtor / ??_7Vftable:名字后可跟嵌套类链(@Outer),再以 @@ 结束名字部分
    m = re.match(r"^\?\?(?:0|1|_7)(\w+)((?:@\w+)*)@@", decorated)
    if m:
        member, chain = m.group(1), [c for c in m.group(2).lstrip("@").split("@") if c]
        if decorated.startswith("??0"):
            return "::".join(chain + [member, member])
        if decorated.startswith("??1"):
            return "::".join(chain + [member, "~" + member])
        return "::".join(chain + [member, "vtable"])
    # 自由函数 ?name@@… 必须先于成员判定:否则紧随其后的返回类型编码会被当成「类名」
    m = re.match(r"^\?(\w+)@@", decorated)
    if m:
        return m.group(1)
    m = re.match(r"^\?(\w+)@((?:\w+@)+)@?", decorated)
    if m:
        classes = [c for c in m.group(2).rstrip("@").split("@") if c]
        return "::".join(list(reversed(classes)) + [m.group(1)])
    return decorated


def qt_keys_linux(path):
    """ELF:未定义符号 − 非 Qt 依赖库的导出符号 = Qt 符号(再 demangle 归一)。"""
    undef = set()
    for line in run(["nm", "-D", "--undefined-only", path]).splitlines():
        parts = line.split()
        # 只看强引用 'U':弱未定义符号(__gmon_start__、_ITM_* 之类)本来就可以不存在
        if len(parts) >= 2 and parts[-2] == "U":
            undef.add(base(parts[-1]))
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", run(["readelf", "-d", path]))
    resolved = {}
    for line in run(["ldd", path]).splitlines():
        m = re.match(r"^\s*(\S+) => (\S+)", line)
        if m:
            resolved[m.group(1)] = m.group(2)
    non_qt = set()
    for lib in needed:
        if lib.startswith("libQt"):
            continue
        real = resolved.get(lib)
        if not real or not os.path.exists(real):
            # 解析不到依赖库(构建目录外的机器):它的符号会被误当成 Qt 符号报出来
            print("warning: cannot resolve %s; its symbols may be reported as Qt" % lib, file=sys.stderr)
            continue
        for line in run(["nm", "-D", "--defined-only", real]).splitlines():
            parts = line.split()
            if len(parts) >= 3:
                non_qt.add(base(parts[-1]))
    qt = sorted(undef - non_qt)
    keys = set()
    if qt:
        demangled = run(["c++filt"] + qt).splitlines()
        for dem in demangled:
            keys.add(key_demangled(dem.strip()))
    return keys, qt


def qt_keys_macos(path):
    """Mach-O:未定义符号 − 非 Qt 依赖动态库的导出符号。"""
    undef = set()
    for line in run(["nm", "-u", path]).splitlines():
        parts = line.split()
        if parts:
            undef.add(base(parts[-1]))
    non_qt = set()
    for line in run(["otool", "-L", path]).splitlines()[1:]:
        lib = line.strip().split(" (compatibility")[0].strip()
        if "Qt" in os.path.basename(lib) or not os.path.exists(lib):
            continue
        for defline in run(["nm", "-gU", lib]).splitlines():
            parts = defline.split()
            if len(parts) >= 3:
                non_qt.add(base(parts[-1]))
    qt = sorted(undef - non_qt)
    keys = set()
    if qt:
        for dem in run(["c++filt"] + qt).splitlines():
            keys.add(key_demangled(dem.strip()))
    return keys, qt


def dumpbin_path():
    """Windows:dumpbin 不在默认 PATH,经 vswhere 找 VS 里的那一份。"""
    from glob import glob
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, "-latest", "-products", "*", "-find",
                              r"VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe"],
                             capture_output=True, text=True).stdout.strip().splitlines()
        if out:
            return out[-1]
    hits = glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe")
    if hits:
        return sorted(hits)[-1]
    return "dumpbin"


def qt_keys_windows(path):
    """PE:/imports 里来自 Qt6*.dll 的导入即 Qt 符号(修饰名归一为 类::成员)。"""
    text = run([dumpbin_path(), "/nologo", "/imports", path])
    keys = set()
    current = None
    for line in text.splitlines():
        m = re.match(r"^\s{4}(\S+\.(?:dll|DLL))\s*$", line)
        if m:
            current = m.group(1)
            continue
        if current and current.lower().startswith("qt6"):
            m = re.match(r"^\s+[0-9A-Fa-f]+\s+(\S+)\s*$", line)
            if m:
                keys.add(key_msvc(m.group(1)))
    return keys, sorted(keys)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    plugin = sys.argv[1]
    allowlist_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "qt-symbol-allowlist.txt")
    allow = set()
    with open(allowlist_path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                allow.add(line)
    if sys.platform.startswith("linux"):
        keys, raw = qt_keys_linux(plugin)
    elif sys.platform == "darwin":
        keys, raw = qt_keys_macos(plugin)
    elif sys.platform in ("win32", "cygwin"):
        keys, raw = qt_keys_windows(plugin)
    else:
        print("unsupported platform %s" % sys.platform)
        return 0
    new = sorted(keys - allow)
    stale = sorted(allow - keys)
    print("Qt symbols referenced: %d (%d raw), allowlist: %d" % (len(keys), len(raw), len(allow)))
    if stale:
        print("note: %d allowlist entries no longer referenced (harmless): %s" % (
            len(stale), ", ".join(stale[:8]) + (" ..." if len(stale) > 8 else "")))
    if new:
        print("NEW Qt symbol(s) not in %s:" % allowlist_path)
        for k in new:
            print("  " + k)
        print("Verify each exists in the oldest supported Qt (see file header), then add it to the allowlist.")
        return 1
    print("OK: every Qt symbol is on the allowlist")
    return 0


def _selftest_msvc():
    """MSVC 修饰名解析的自测(CI 的 Windows job 才会真的遇到它们,这里先兜住明显错误)。"""
    cases = {
        "?setText@QLabel@@QEAAXAEBVQString@@@Z": "QLabel::setText",
        "??0QLabel@@QEAA@AEBVQString@@PEAVQWidget@@V?$QFlags@W4WindowType@Qt@@@@I@Z": "QLabel::QLabel",
        "??1QLabel@@UEAA@XZ": "QLabel::~QLabel",
        "??_7QPixmap@@6B@": "QPixmap::vtable",
        "?staticMetaObject@QTimer@@2UQMetaObject@@B": "QTimer::staticMetaObject",
        "?clicked@QAbstractButton@@QEAAX_N@Z": "QAbstractButton::clicked",
        "?connectImpl@QObject@@IEAA?AVQMetaObject@Qt@@PEBVQObject@@PEBD1@Z": "QObject::connectImpl",
        "?qt_qFindChild_helper@@YAHPEBVQObject@@AEBVQString@@@Z": "qt_qFindChild_helper",
        "?exec@QDialog@@QEAAHXZ": "QDialog::exec",
        "??1Connection@QMetaObject@@QEAA@XZ": "QMetaObject::Connection::~Connection",
        "??0QWidget@@QEAA@PEAV0@V?$QFlags@W4WindowType@Qt@@@@I@Z": "QWidget::QWidget",
        "?deallocate@QArrayData@@SAXPEAX_J1@Z": "QArrayData::deallocate",
        "?fromUtf8@QString@@SA?AV1@VQByteArrayView@Qt@@@Z": "QString::fromUtf8",
        "?getAndRef@ExternalRefCountData@QtSharedPointer@@SAPEAV12@PEBVQObject@@@Z":
            "QtSharedPointer::ExternalRefCountData::getAndRef",
    }
    for decorated, want in cases.items():
        got = key_msvc(decorated)
        if got != want:
            raise AssertionError("MSVC key mismatch: %r -> %r (want %r)" % (decorated, got, want))
    return len(cases)


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        print("msvc selftest ok (%d cases)" % _selftest_msvc())
        sys.exit(0)
    sys.exit(main() or 0)
