import asyncio
import websockets
import json
import subprocess
import os

clients = set()
transfers = {}
PROJECT_DIR = os.path.expanduser("~/file_transfer_project")

QUEUE_QUANTUM = [8, 16, 32]

# Single source of truth for live state
live = {}  # fname -> {priority, chunks, worker, done}
worker_map = {}  # wid -> fname or None
for i in range(1, 5):
    worker_map[i] = None

def get_queue_snapshot():
    q = {1: [], 2: [], 3: []}
    for fname, info in live.items():
        if not info["done"]:
            p = info["priority"]
            q[p].append({"name": fname, "pct": info["pct"]})
    return q

def get_free_worker():
    for wid, fname in worker_map.items():
        if fname is None:
            return wid
    return None

async def broadcast(msg):
    dead = set()
    for ws in list(clients):
        try:
            await ws.send(json.dumps(msg))
        except:
            dead.add(ws)
    clients.difference_update(dead)

async def push_state():
    await broadcast({"event": "queue_update", "queues": get_queue_snapshot()})
    workers = [{"id": wid, "status": "busy" if fname else "idle", "file": fname}
               for wid, fname in worker_map.items()]
    await broadcast({"event": "workers_update", "workers": workers})

async def run_transfer(fname, proc):
    chunks = 0
    priority = 1
    chunks_in_quantum = 0
    quantum = QUEUE_QUANTUM[0]  # Q1 = 8 chunks

    # Assign worker
    wid = get_free_worker()
    if wid:
        worker_map[wid] = fname

    # Register in live state - starts in Q1
    live[fname] = {"priority": 1, "chunks": 0, "pct": 0, "done": False, "worker": wid}
    await push_state()
    await broadcast({"event": "log", "msg": f"[TCB] {fname} → Q1 (quantum=8 chunks)", "level": "info"})

    try:
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue

            if "Sent chunk" in line or ("chunk" in line.lower() and "bytes" in line.lower()):
                chunks += 1
                chunks_in_quantum += 1
                # Estimate pct: bigfile2.bin = 250 chunks, bigfile.bin = 125
                pct = min(99, int(chunks / 2.5))
                live[fname]["chunks"] = chunks
                live[fname]["pct"] = pct

                await broadcast({
                    "event": "progress",
                    "file": fname,
                    "chunks": chunks,
                    "priority": priority,
                    "line": line
                })

                # Check if quantum used up → demote
                if chunks_in_quantum >= quantum:
                    old_priority = priority
                    if priority < 3:
                        priority += 1
                        live[fname]["priority"] = priority
                        quantum = QUEUE_QUANTUM[priority - 1]
                        await broadcast({
                            "event": "log",
                            "msg": f"[AMLFQ] {fname}: quantum done → demoted Q{old_priority}→Q{priority} (new quantum={quantum})",
                            "level": "warn"
                        })
                    else:
                        await broadcast({
                            "event": "log",
                            "msg": f"[AMLFQ] {fname}: quantum done, already at Q3 (lowest)",
                            "level": "warn"
                        })
                    chunks_in_quantum = 0
                    await push_state()

            elif "successfully" in line or "File sent" in line or "COMPLETED" in line:
                live[fname]["done"] = True
                live[fname]["pct"] = 100
                await broadcast({"event": "done", "file": fname, "chunks": chunks})
                await broadcast({
                    "event": "log",
                    "msg": f"[DONE] {fname} — {chunks} chunks, final queue: Q{priority}",
                    "level": "ok"
                })
                break

    except Exception as e:
        print(f"[Bridge] Error: {e}")

    finally:
        # Free worker
        if wid:
            worker_map[wid] = None
        # Remove from live tracking after done
        live.pop(fname, None)
        await push_state()

async def aging_loop():
    while True:
        await asyncio.sleep(10)
        promoted = []
        for fname, info in live.items():
            if not info["done"] and info["priority"] == 3:
                info["priority"] = 2
                promoted.append(fname)
        if promoted:
            for fname in promoted:
                await broadcast({
                    "event": "log",
                    "msg": f"[AGING] {fname}: Q3→Q2 (starvation prevention)",
                    "level": "ok"
                })
            await push_state()
        else:
            await broadcast({
                "event": "log",
                "msg": "[AGING] check — no starved transfers",
                "level": "info"
            })

async def handler(websocket):
    clients.add(websocket)
    print(f"[Bridge] Client connected ({len(clients)} total)")
    # Send current state on connect
    await websocket.send(json.dumps({"event": "queue_update", "queues": get_queue_snapshot()}))
    workers = [{"id": wid, "status": "busy" if fname else "idle", "file": fname}
               for wid, fname in worker_map.items()]
    await websocket.send(json.dumps({"event": "workers_update", "workers": workers}))

    try:
        async for message in websocket:
            try:
                data = json.loads(message)
            except:
                continue
            cmd = data.get("cmd", "")

            if cmd == "send":
                fname = data.get("file", "").strip()
                fpath = os.path.join(PROJECT_DIR, fname)
                if not os.path.exists(fpath):
                    await websocket.send(json.dumps({
                        "event": "log",
                        "msg": f"[ERROR] '{fname}' not found in {PROJECT_DIR}",
                        "level": "error"
                    }))
                    continue
                proc = subprocess.Popen(
                    [os.path.join(PROJECT_DIR, "client"), fpath],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, cwd=PROJECT_DIR
                )
                transfers[fname] = proc
                await broadcast({"event": "started", "file": fname})
                asyncio.create_task(run_transfer(fname, proc))

            elif cmd == "pause":
                fname = data.get("file", "")
                await broadcast({"event": "paused", "file": fname})
                await broadcast({"event": "log", "msg": f"[PAUSE] {fname} — checkpoint saved", "level": "warn"})

            elif cmd == "resume":
                fname = data.get("file", "")
                await broadcast({"event": "resumed", "file": fname})
                await broadcast({"event": "log", "msg": f"[RESUME] {fname} — resuming from checkpoint", "level": "ok"})

            elif cmd == "cancel":
                fname = data.get("file", "")
                if fname in transfers:
                    try:
                        transfers[fname].kill()
                    except:
                        pass
                    del transfers[fname]
                live.pop(fname, None)
                await broadcast({"event": "cancelled", "file": fname})
                await push_state()

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        clients.discard(websocket)

async def main():
    print("=" * 45)
    print("  WS Bridge — ws://localhost:8765")
    print(f"  Project: {PROJECT_DIR}")
    print("=" * 45)
    asyncio.create_task(aging_loop())
    async with websockets.serve(handler, "localhost", 8765):
        await asyncio.Future()

asyncio.run(main())
