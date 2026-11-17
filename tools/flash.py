#!/usr/bin/env python3
"""Flash whichever board is attached: reads the chip id through its ST-LINK, builds <radio>_<chip>, programs it.

    python tools/flash.py                       # rylr999 firmware, node id 1, on the only attached board
    python tools/flash.py lr1121 --node-id 2    # another radio and node id
    python tools/flash.py --serial 0670FF56...  # one of several attached boards (see --list)
    python tools/flash.py --chip f103           # skip the chip id read, for a board that does not answer it
    python tools/flash.py --list                # attached ST-LINKs and the chip behind each

Needs only PlatformIO; OpenOCD comes from its tool-openocd package. PlatformIO's own upload cannot pick one of several
ST-LINKs, so this programs with OpenOCD directly, by the probe's serial number. ST-LINKs are found by their serial
ports; a dongle without one (the usual probe for a Blue Pill) is used when it is the only ST-LINK attached.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
RADIOS = ("rylr999", "rylr498", "lr1121", "none")
STLINK_VID = "0483"
STLINK_PIDS = {"3748", "374B", "374E", "374F", "3752", "3753", "3754"}  # V2, V2-1, V3 variants
# CPUTAPID 0 accepts any debug port id: Blue Pill clones often answer with another vendor's
F1_TARGET = ["-c", "set CPUTAPID 0", "-f", "target/stm32f1x.cfg"]
# DBGMCU_IDCODE DEV_ID -> (environment suffix, OpenOCD arguments to program it, chip name)
CHIPS = {
    0x421: ("f446", ["-f", "board/st_nucleo_f4.cfg"], "STM32F446"),
    0x415: ("l476", ["-f", "board/st_nucleo_l4.cfg"], "STM32L476/486"),
    0x410: ("f103", ["-f", "interface/stlink.cfg", "-c", "transport select swd"] + F1_TARGET, "STM32F103x8/xB"),
}
DBGMCU_IDCODE = 0xE0042000  # same address on the F1, the F4 and the L4


def core_dir():
    return Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))


def find_pio():
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return found
    for name in ("Scripts/pio.exe", "bin/pio"):
        candidate = core_dir() / "penv" / name
        if candidate.exists():
            return str(candidate)
    sys.exit("PlatformIO not found: install it or put pio on PATH")


def find_openocd():
    pkg = core_dir() / "packages" / "tool-openocd"
    exe = pkg / "bin" / ("openocd.exe" if os.name == "nt" else "openocd")
    if not exe.exists():
        sys.exit(f"OpenOCD not found at {exe}; build any environment once so PlatformIO installs it")
    return str(exe), str(pkg / "openocd" / "scripts")


def list_stlinks(pio):
    out = subprocess.run([pio, "device", "list", "--serial", "--json-output"], capture_output=True, text=True,
                         check=True).stdout
    probes = []
    for dev in json.loads(out):
        m = re.search(r"VID:PID=([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})(?:\s+SER=(\S+))?", dev.get("hwid", ""))
        if m and m.group(1).upper() == STLINK_VID and m.group(2).upper() in STLINK_PIDS and m.group(3):
            probes.append({"serial": m.group(3), "port": dev["port"]})
    return probes


def adapter(serial):
    return ["-c", f"adapter serial {serial}"] if serial else []


def probe_chip(openocd, scripts, serial):
    """DEV_ID of the chip behind this ST-LINK, or None. The F4 target config can read either Nucleo's chip; an F1
    has a different debug port id, so it is tried with its own."""
    for target in (["-f", "target/stm32f4x.cfg"], F1_TARGET):
        cmd = [openocd, "-s", scripts, "-f", "interface/stlink.cfg", *adapter(serial), *target,
               "-c", "init", "-c", f"mdw 0x{DBGMCU_IDCODE:08x}", "-c", "shutdown"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        m = re.search(r"0x%08x:\s+([0-9a-fA-F]{8})" % DBGMCU_IDCODE, res.stdout + res.stderr)
        if m:
            return int(m.group(1), 16) & 0xFFF
    return None


def describe(dev_id):
    if dev_id is None:
        return "no response (board powered and SWD free?)"
    chip = CHIPS.get(dev_id)
    return f"{chip[2]} -> *_{chip[0]}" if chip else f"unsupported chip, DEV_ID 0x{dev_id:03x}"


def label(probe):
    if probe["port"]:
        return f"{probe['port']} ({probe['serial']})"
    return f"ST-LINK {probe['serial']}" if probe["serial"] else "ST-LINK without a serial port"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("radio", nargs="?", default="rylr999", choices=RADIOS, help="radio driver (default rylr999)")
    ap.add_argument("--node-id", type=int, help="NODE_ID, 1 to 254, unique per board (firmware default 1)")
    ap.add_argument("--station-id", help="STATION_ID callsign sent in status frames")
    ap.add_argument("--serial", help="ST-LINK serial number, when more than one board is attached")
    ap.add_argument("--chip", choices=sorted(c[0] for c in CHIPS.values()),
                    help="build for this chip without reading its id")
    ap.add_argument("--list", action="store_true", help="list attached ST-LINKs and their chips, then stop")
    args = ap.parse_args()

    pio = find_pio()
    openocd, scripts = find_openocd()
    probes = list_stlinks(pio)

    no_port = {"serial": None, "port": None}

    if args.list:
        for p in probes:
            print(f"{p['serial']}  {p['port']}  {describe(probe_chip(openocd, scripts, p['serial']))}")
        if not probes:
            dev_id = probe_chip(openocd, scripts, None)
            print("no ST-LINK attached" if dev_id is None else f"(no serial port)  {describe(dev_id)}")
        return

    if args.serial:
        chosen = next((p for p in probes if p["serial"].lower() == args.serial.lower()),
                      {"serial": args.serial, "port": None})
    elif len(probes) == 1:
        chosen = probes[0]
    elif not probes:
        chosen = no_port
    else:
        sys.exit("several ST-LINKs attached; pick one with --serial:\n" +
                 "\n".join(f"  {p['serial']}  {p['port']}" for p in probes))

    if args.chip:
        dev_id = next(i for i, c in CHIPS.items() if c[0] == args.chip)
    else:
        dev_id = probe_chip(openocd, scripts, chosen["serial"])
        if dev_id is None and chosen is no_port:
            sys.exit("no ST-LINK attached, or the board behind it did not answer (powered and SWD wired?)")
        if dev_id not in CHIPS:
            sys.exit(f"{label(chosen)}: {describe(dev_id)}; --chip sets the chip by hand")
    suffix, target_cfg, chip_name = CHIPS[dev_id]
    env_name = f"{args.radio}_{suffix}"
    print(f"{label(chosen)}: {chip_name}, building {env_name}")

    env = dict(os.environ)
    flags = [env["PLATFORMIO_BUILD_FLAGS"]] if env.get("PLATFORMIO_BUILD_FLAGS") else []
    if args.node_id is not None:
        if not 1 <= args.node_id <= 254:
            sys.exit("--node-id must be 1 to 254")
        flags.append(f"-DNODE_ID={args.node_id}")
    if args.station_id is not None:
        flags.append(f'-DSTATION_ID=\\"{args.station_id}\\"')
    if flags:
        env["PLATFORMIO_BUILD_FLAGS"] = " ".join(flags)
    if subprocess.run([pio, "run", "-e", env_name], cwd=PROJECT, env=env).returncode != 0:
        sys.exit("build failed")

    elf = (PROJECT / ".pio" / "build" / env_name / "firmware.elf").as_posix()
    cmd = [openocd, "-s", scripts, *target_cfg, *adapter(chosen["serial"]),
           "-c", f"program {{{elf}}} verify reset exit"]
    if subprocess.run(cmd).returncode != 0:
        sys.exit("programming failed")
    node = args.node_id if args.node_id is not None else "1 (default)"
    print(f"flashed {env_name} to {label(chosen)}, NODE_ID {node}")


if __name__ == "__main__":
    main()
