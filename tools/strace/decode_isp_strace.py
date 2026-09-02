#!/usr/bin/env python3
"""Decode a strace of mediaserver on mocha: keep only camera/ISP-relevant
device access, resolve ioctl command numbers against the stock kernel's
nvhost/nvmap/vi/isp headers, track fd -> path per line order.

Usage:
    python3 decode_isp_strace.py mediaserver.strace > filtered.txt

The trace must be taken with:
    strace -f -e trace=open,openat,ioctl,mmap,mmap2,write,pwrite64,close \
           -s 256 -o mediaserver.strace -p <pid of mediaserver>
"""
import re
import sys

IOCTL_TABLE = {
    0xC0184106: "NVHOST_AS_IOCTL_ALLOC_SPACE",
    0xC0044101: "NVHOST_AS_IOCTL_BIND_CHANNEL",
    0xC0104103: "NVHOST_AS_IOCTL_FREE_SPACE",
    0xC0184104: "NVHOST_AS_IOCTL_MAP_BUFFER",
    0xC0084105: "NVHOST_AS_IOCTL_UNMAP_BUFFER",
    0x40084864: "NVHOST_IOCTL_CHANNEL_ALLOC_GPFIFO",
    0xC010486C: "NVHOST_IOCTL_CHANNEL_ALLOC_OBJ_CTX",
    0xC004486A: "NVHOST_IOCTL_CHANNEL_CYCLE_STATS",
    0x8008486D: "NVHOST_IOCTL_CHANNEL_FREE_OBJ_CTX",
    0x80084809: "NVHOST_IOCTL_CHANNEL_GET_CLK_RATE",
    0xC0084817: "NVHOST_IOCTL_CHANNEL_GET_MODMUTEX",
    0x80044804: "NVHOST_IOCTL_CHANNEL_GET_MODMUTEXES",
    0xC0084810: "NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT",
    0x80044802: "NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS",
    0x8004480C: "NVHOST_IOCTL_CHANNEL_GET_TIMEDOUT",
    0xC0084811: "NVHOST_IOCTL_CHANNEL_GET_WAITBASE",
    0x80044803: "NVHOST_IOCTL_CHANNEL_GET_WAITBASES",
    0x80044806: "NVHOST_IOCTL_CHANNEL_NULL_KICKOFF",
    0xC0084808: "NVHOST_IOCTL_CHANNEL_READ_3D_REG",
    0x4008480A: "NVHOST_IOCTL_CHANNEL_SET_CLK_RATE",
    0xC0384819: "NVHOST_IOCTL_CHANNEL_SET_CTXSWITCH",
    0xC018486F: "NVHOST_IOCTL_CHANNEL_SET_ERROR_NOTIFIER",
    0x40044805: "NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD",
    0x4004480D: "NVHOST_IOCTL_CHANNEL_SET_PRIORITY",
    0x4004480B: "NVHOST_IOCTL_CHANNEL_SET_TIMEOUT",
    0xC0084812: "NVHOST_IOCTL_CHANNEL_SET_TIMEOUT_EX",
    0xC078481A: "NVHOST_IOCTL_CHANNEL_SUBMIT",
    0xC018486B: "NVHOST_IOCTL_CHANNEL_SUBMIT_GPFIFO",
    0xC0184866: "NVHOST_IOCTL_CHANNEL_WAIT",
    0xC010486E: "NVHOST_IOCTL_CHANNEL_ZCULL_BIND",
    0x80044807: "NVHOST_IOCTL_CTRL_GET_VERSION",
    0xC0084804: "NVHOST_IOCTL_CTRL_MODULE_MUTEX",
    0xC020480C: "NVHOST_IOCTL_CTRL_MODULE_REGRDWR",
    0xC018480B: "NVHOST_IOCTL_CTRL_SYNC_FENCE_CREATE",
    0x40044802: "NVHOST_IOCTL_CTRL_SYNCPT_INCR",
    0xC0084801: "NVHOST_IOCTL_CTRL_SYNCPT_READ",
    0xC0084808: "NVHOST_IOCTL_CTRL_SYNCPT_READ_MAX",
    0x400C4803: "NVHOST_IOCTL_CTRL_SYNCPT_WAIT",
    0xC0104806: "NVHOST_IOCTL_CTRL_SYNCPT_WAITEX",
    0xC0204809: "NVHOST_IOCTL_CTRL_SYNCPT_WAITMEX",
    0x40104901: "NVHOST_ISP_IOCTL_SET_EMC",
    0x40045601: "NVHOST_VI_IOCTL_ENABLE_TPG",
    0x40045602: "NVHOST_VI_IOCTL_SET_EMC_INFO",
    0xC0184102: "NVHOST32_AS_IOCTL_ALLOC_SPACE",
    0xC044480F: "NVHOST32_IOCTL_CHANNEL_SUBMIT",
    0xC0184805: "NVHOST32_IOCTL_CTRL_MODULE_REGRDWR",
    0xC020480A: "NVHOST32_IOCTL_CTRL_SYNC_FENCE_CREATE",
    0x40184E03: "NVMAP_IOC_ALLOC",
    0x40184E64: "NVMAP_IOC_ALLOC_KIND",
    0x40184E0C: "NVMAP_IOC_CACHE",
    0x40204E11: "NVMAP_IOC_CACHE_LIST",
    0xC0104E01: "NVMAP_IOC_CLAIM",
    0xC0104E00: "NVMAP_IOC_CREATE",
    0x00004E04: "NVMAP_IOC_FREE",
    0xC0104E10: "NVMAP_IOC_FROM_FD",
    0xC0104E02: "NVMAP_IOC_FROM_ID",
    0xC0104E0F: "NVMAP_IOC_GET_FD",
    0xC0104E0D: "NVMAP_IOC_GET_ID",
    0xC0204E05: "NVMAP_IOC_MMAP",
    0xC0184E08: "NVMAP_IOC_PARAM",
    0xC0184E0A: "NVMAP_IOC_PIN_MULT",
    0x40284E07: "NVMAP_IOC_READ",
    0xC0104E0E: "NVMAP_IOC_SHARE",
    0x40184E0B: "NVMAP_IOC_UNPIN_MULT",
    0x40284E06: "NVMAP_IOC_WRITE",
}

INTERESTING = re.compile(
    r"^/dev/(nvhost|nvmap|mipi-cal|tegra_camera|camera|i2c|mem)"
    r"|^/sys/"
    r"|^/proc/")

LINE = re.compile(
    r"^(?:(?P<pid>\d+)\s+)?"
    r"(?P<call>openat|open|ioctl|mmap2|mmap|write|pwrite64|close)"
    r"\((?P<args>.*)\)\s*=\s*(?P<ret>.*?)(?:\s|$)")

STRIP = re.compile(r"^(?P<pid>\d+)\s{2,}(?P<rest>.*)$")

QUOTED = re.compile(r'"([^"]*)"')


def first_string(args):
    m = QUOTED.search(args)
    return m.group(1) if m else None


def first_int(args):
    m = re.match(r"\s*(-?\d+|0x[0-9a-f]+|AT_FDCWD|-?AT_FDCWD)", args)
    if not m:
        return None
    t = m.group(1)
    if t.startswith("AT_FDCWD"):
        return -100
    return int(t, 0)


def split_args(args):
    """Split a strace argument list on top-level commas."""
    out, depth, cur, inq = [], 0, [], False
    for ch in args:
        if inq:
            cur.append(ch)
            if ch == '"':
                inq = False
            continue
        if ch == '"':
            inq = True
            cur.append(ch)
            continue
        if ch in "(<[{" or ch == "[":
            depth += 1
            cur.append(ch)
            continue
        if ch in ")>]}" or ch == "]":
            depth -= 1
            cur.append(ch)
            continue
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    out.append("".join(cur).strip())
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    fd_path = {}
    out = sys.stdout
    idx = 0
    for raw in open(sys.argv[1], errors="replace"):
        line = raw.rstrip("\n")
        m = STRIP.match(line)
        if m:
            pid = m.group("pid")
            rest = m.group("rest")
        else:
            pid = "-"
            rest = line
        lm = LINE.match(rest)
        if not lm:
            continue
        call = lm.group("call")
        args = lm.group("args")
        parts = split_args(args)
        idx += 1

        if call in ("open", "openat"):
            path = first_string(args)
            fd = None
            if call == "openat" and len(parts) > 2:
                fd = parts[2].strip()
            if path and INTERESTING.match(path):
                fd_path[fd] = path
                print("[%06d] %s %s open%s -> fd %s  %s"
                      % (idx, pid, call,
                         "_at" if call == "openat" else "", fd, path),
                      file=out)
            continue

        if call == "close":
            fd = first_int(args)
            if fd is not None and fd in fd_path:
                print("[%06d] %s close fd %d  %s" % (idx, fd, fd_path[fd]),
                      file=out)
                fd_path.pop(fd, None)
            continue

        if call in ("ioctl", "write", "pwrite64", "mmap", "mmap2"):
            fd = first_int(args)
            if fd is None or fd not in fd_path:
                continue
            path = fd_path[fd]
            if not INTERESTING.match(path):
                continue
            if call == "ioctl":
                mm = re.search(r"0x[0-9a-fA-F]+", args)
                cmd = int(mm.group(0), 16) if mm else None
                name = IOCTL_TABLE.get(cmd, "UNKNOWN")
                extra = parts[2] if len(parts) > 2 else ""
                print("[%06d] %s ioctl %s  %s  %s" % (idx, path, name,
                                                      extra[:60], args[:140]),
                      file=out)
            elif call in ("write", "pwrite64"):
                print("[%06d] %s %s fd %d  %s" % (idx, call, fd, path,
                                                  args[:80]),
                      file=out)
            else:
                print("[%06d] %s %s fd %s  args: %s" % (idx, call, fd, args),
                      file=out)
            continue


if __name__ == "__main__":
    sys.exit(main())
