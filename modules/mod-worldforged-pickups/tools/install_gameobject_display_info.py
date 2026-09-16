#!/usr/bin/env python3
"""Teach the server the gameobject display ids the CoA client already knows.

Why this is needed
------------------
A spawn whose template `displayId` is not in the server's
`Data/dbc/GameObjectDisplayInfo.dbc` is thrown away at load:

    Gameobject (GUID: 6900001 Entry 1344099 GoType: 3) has an invalid displayId (87226), not loaded.

A stock 3.3.5a table holds 3,792 rows. CoA's own client ships an extended one (120,871
rows) with every custom model the realm uses, and the restored Worldforged pickups use
1,009 display ids from it. Against the stock table 1,489 of the 1,510 restored pickups
never reach the world - the SQL is right, the world is still empty - and 1,222 unrelated
stock objects are dropped the same way. This is a prerequisite, not an optional nicety.

What it does
------------
Merges, never replaces. The server's own rows stay exactly as they are (a few dozen of its
ids are not in the client table, and the stock bounds this server was tuned with are kept),
and every client row whose id the server lacks is appended verbatim - same layout, so the
model path and the bounds the core uses for `GameObject::IsInRange2d/3d` come from the client
itself instead of being invented. A backup is written first and the result is re-read and
verified before the script reports success.

Usage
-----
    # straight from the client archive (needs: pip install mpyq)
    python tools/install_gameobject_display_info.py --client "C:/Games/CoA/client"

    # or point at a DBC you already extracted with any MPQ tool
    python tools/install_gameobject_display_info.py --client-dbc path/to/GameObjectDisplayInfo.dbc

    # where is the server's copy?
    python tools/install_gameobject_display_info.py --server "C:/CoA/Data/dbc/GameObjectDisplayInfo.dbc"

Both `--client` and `--server` also read COA_CLIENT and COA_SERVER_DBC. `--check` reports
what would change and writes nothing. Restart the worldserver afterwards; the server reads
this table only at startup.
"""

import argparse
import datetime
import json
import os
import shutil
import struct
import sys

MEMBER = "DBFilesClient\\GameObjectDisplayInfo.dbc"
DEFAULT_CLIENT = os.environ.get("COA_CLIENT", "")
DEFAULT_SERVER = os.environ.get(
    "COA_SERVER_DBC", os.path.join("Data", "dbc", "GameObjectDisplayInfo.dbc"))


def read_dbc(path):
    """Parse a WDBC file into raw records + its string block (layout is not interpreted)."""
    with open(path, "rb") as fh:
        raw = fh.read()
    if raw[:4] != b"WDBC":
        raise SystemExit(f"{path}: not a WDBC file (magic {raw[:4]!r})")
    _, recs, fields, rsize, ssize = struct.unpack("<4sIIII", raw[:20])
    if 20 + recs * rsize + ssize != len(raw):
        raise SystemExit(f"{path}: header says {20 + recs * rsize + ssize} bytes, file is {len(raw)}")
    return {
        "records": raw[20:20 + recs * rsize],
        "strings": raw[20 + recs * rsize:],
        "ids": [struct.unpack("<I", raw[20 + i * rsize:24 + i * rsize])[0] for i in range(recs)],
        "fields": fields,
        "rsize": rsize,
    }


def extract_from_archive(client_dir, archive_names=("patch-M.MPQ", "patch-W.MPQ", "patch-X.MPQ")):
    """Pull the DBC out of the client's archives, newest patch first."""
    try:
        import mpyq
    except ImportError:
        raise SystemExit("mpyq is not installed (pip install mpyq), or pass --client-dbc")
    data_dir = os.path.join(client_dir, "Data")
    seen = []
    for name in archive_names:
        path = os.path.join(data_dir, name)
        if not os.path.exists(path):
            continue
        try:
            data = mpyq.MPQArchive(path).read_file(MEMBER)
        except Exception as exc:                                  # archive may not hold it
            seen.append(f"{name}: {type(exc).__name__}")
            continue
        if data and data[:4] == b"WDBC":
            print(f"  read {len(data):,} bytes from {name}")
            return data
        seen.append(f"{name}: no {MEMBER}")
    raise SystemExit("could not read the DBC from the client archives (" + ", ".join(seen) +
                     "); extract it with an MPQ tool and pass --client-dbc")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", default=DEFAULT_CLIENT, help="the CoA client folder")
    ap.add_argument("--client-dbc", help="an already extracted GameObjectDisplayInfo.dbc")
    ap.add_argument("--server", default=DEFAULT_SERVER, help="the server's Data/dbc copy")
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    a = ap.parse_args()

    if a.client_dbc:
        with open(a.client_dbc, "rb") as fh:
            client_raw = fh.read()
        print(f"client table: {a.client_dbc}")
    elif a.client:
        print(f"client table: {a.client}, {MEMBER}")
        client_raw = extract_from_archive(a.client)
    else:
        raise SystemExit("pass --client or --client-dbc (COA_CLIENT also works)")

    if not os.path.exists(a.server):
        raise SystemExit(f"server table not found: {a.server} (pass --server)")
    server = read_dbc(a.server)

    tmp = os.path.join(os.path.dirname(os.path.abspath(a.server)), ".client-extract.tmp")
    with open(tmp, "wb") as fh:
        fh.write(client_raw)
    client = read_dbc(tmp)
    os.unlink(tmp)

    print(f"server: {len(server['ids']):,} rows   client: {len(client['ids']):,} rows")
    if (server["fields"], server["rsize"]) != (client["fields"], client["rsize"]):
        raise SystemExit(f"layouts differ (server {server['fields']}x{server['rsize']}, "
                         f"client {client['fields']}x{client['rsize']}) - refusing to copy rows")

    have = set(server["ids"])
    missing = [i for i, rid in enumerate(client["ids"]) if rid not in have]
    print(f"ids the server lacks: {len(missing):,}")
    if not missing:
        print("nothing to do")
        return 0
    if a.check:
        print("--check: nothing written")
        return 0

    # Keep the server's string block in front so its existing offsets stay valid; the copied
    # rows get their string offset shifted by the length of that block.
    shift = len(server["strings"])
    records = bytearray(server["records"])
    for idx in missing:
        row = bytearray(client["records"][idx * client["rsize"]:(idx + 1) * client["rsize"]])
        row[4:8] = struct.pack("<I", struct.unpack("<I", row[4:8])[0] + shift)
        records += row
    strings = server["strings"] + client["strings"]
    total = len(records) // client["rsize"]

    backup_dir = os.path.join(os.path.dirname(os.path.abspath(a.server)), "backup")
    os.makedirs(backup_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = os.path.join(backup_dir, os.path.basename(a.server) + f".bak-{stamp}")
    shutil.copy2(a.server, backup)

    with open(a.server, "wb") as fh:
        fh.write(struct.pack("<4sIIII", b"WDBC", total, client["fields"], client["rsize"],
                             len(strings)) + bytes(records) + strings)

    back = read_dbc(a.server)
    offsets_ok = all(0 <= struct.unpack("<I", back["records"][i * client["rsize"] + 4:
                                                              i * client["rsize"] + 8])[0]
                     < len(back["strings"])
                     for i in range(0, len(back["ids"]), 997))
    print(f"wrote {a.server}")
    print(f"  rows {len(back['ids']):,}  parse ok {len(back['ids']) == total}  "
          f"string offsets ok {offsets_ok}")
    print(f"  backup {backup}")

    manifest = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                            "data", "gameobject_display_info.manifest.json")
    with open(os.path.abspath(manifest), "w", encoding="utf-8") as fh:
        json.dump({"installed_at": stamp, "server_file": os.path.abspath(a.server),
                   "client_source": a.client_dbc or a.client, "rows_before": len(server["ids"]),
                   "rows_added": len(missing), "rows_after": len(back["ids"]),
                   "backup": backup}, fh, indent=2)
    print(f"  manifest {os.path.abspath(manifest)}")
    print("\nRestart the worldserver, then check the log: a spawn the core dropped reads")
    print("  Gameobject (GUID: ...) has an invalid displayId (...), not loaded.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
