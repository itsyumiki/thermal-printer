# BUG: 1000 µs MOVE/ROW can wedge forever; the 5 s legacy watchdog then kills the TBP session

Status: **open, root cause found for the wedge (reproduced in a host-side simulation), not yet fixed.** Nothing here is hardware-proven beyond the captured trace. No firmware fix has been written or flashed.

- Reported: 2026-11-03 by the owner (bench testing, Bun host + real printer, `/dev/cu.usbmodem101`).
- Code under test: `pinbridge` branch at `da115f8` (CI run 41596643572 green). The device's HELLO reports `build_id = 0`, so the flashed firmware could not be matched to a commit. Treat "device runs da115f8" as unverified.
- Host debug logging added afterwards in `8a1272e` (`--debug` / `PINBRIDGE_DEBUG=1`), CI run 41600595358 green. It is host-only; firmware is unchanged.

## 1. Symptoms (as reported)

1. A step period of 1000 µs is much more comfortable as the default (currently 6000 µs in several places).
2. The Bun host prints no useful logs on the CLI.
3. After a few basic operations (paper feed, paper retract) the printer stops responding and times out; the owner then has to reconnect USB.

Point 2 is addressed by `8a1272e`. Points 1 and 3 are linked: 1000 µs is the only period at which the wedge below is reachable.

## 2. Evidence

### 2.1 Captured trace (host `--debug`, 11:14:34–11:14:49, local time)

Frame format: `id op len data… crc16`. Decodes below were done by hand from the spec/`tbp.ts`.

| Time | Frame | Meaning |
|---|---|---|
| 41.056 | `TX id=6 op=11 … 00000050 03E8` | MOVE +80 steps @ 1000 µs |
| 41.223 | `RX id=6 op=82 …04 03 03 03E8` | DONE (reports effective period 1000) — ~170 ms total, normal |
| 42.005–42.174 | id 9 MOVE +80 then id 10 COILS release | also normal |
| **42.755** | `TX id=11 op=11 … 00000050 03E8` | MOVE +80 @ 1000 µs |
| 42.758 | `RX id=11 op=81 …01` | ACK, queue=1 |
| 43.739 / 44.272 / 44.423 | `TX id=12/13/14 op=11 … FFFFFFB0 03E8` | three MOVE −80 (retracts) |
| 42.758–44.425 | ACKs with queue 2, 3, 4 | window of 4 is now full |
| **43.753** | `RX id=11 op=82 0B82 05 06 00000000` | **PROGRESS, remaining = 0**, ~1 s after start. A +80 move should take ~80 ms. Repeats every second; never DONE |
| 44.733 onward | `RX id=12/13/14 op=82 … 01 xx 04` | periodic QUEUED re-reports |
| 44.423 | last host TX (id 14) | host sends nothing more: keepalive STAT is suppressed while commands are pending, and the window is full anyway |
| **49.415–49.416** | `RX id=11..14 op=82 … 02 05 03` | all four **CANCELLED, reason 03 (FAIL_STOP)** at the same instant |
| **49.417** | `RX? "!wdg" (Invalid TBP hex frame)` | raw non-TBP line from firmware |
| 49.417 | `TX id=15 op=11 …` | next MOVE |
| 49.421 | `RX id=15 op=83 0F83 02 0A 00` | **ERR 0x0A = NOSESSION** |
| 49.422 | `DROP TBP command rejected while commands are waiting for credit` | host tears the link down and reconnects |

49.415 − 44.423 = 4.99 s ≈ the firmware's `wdg = 5000` ms (`pb_core.h:339`).

### 2.2 Earlier owner log (UI Event stream, before `--debug` existed)

- Two sessions hung ~27–30 s after connect with `Job #N: progress · 00 00 00 00` repeating each second, the queue filling to 4/4, then `UART closed by device` / `No UART device found` and a reconnect. Same signature as 2.1.
- **One session is NOT explained by this bug**: at 10:51:42 a command was ACKed with queue=1, then `cancelled · 03` in the same second, then an ignored non-TBP line, then `TBP ERR 0x0a` and a reconnect. There was no wedged job before it, so a 5 s idle watchdog cannot be the trigger there. See open questions.

## 3. Root causes

### 3.1 Primary: off-by-one target when the 1 ms ISR runs inside the start window (firmware)

Files: `firmware/apps/pinbridge/src/pb_printer.h`, `pb_core.h`, `app.c`.

- `pb_step_timer` (`app.c:272`, registered with `sys_s_hi_timer_add(NULL, pb_step_timer, 1)` at `app.c:295`) calls `pb_step_tick` from a 1 ms timer interrupt. The command task (`pb_poll` → `pb_tbp_exec_poll`) runs at normal priority. Nothing makes the start sequence atomic with respect to the ISR.
- `pb_step_feed_schedule` (`pb_printer.h:159`), idle branch (~`:196-201`):
  1. `pb_step_start(c, reverse, steps + 1u, 0u, period_us)` (`:139`) sets `step_active = 1`, `step_wait_ms = 1` (for a 1000 µs period), then calls `pb_drive_motor_phase` (several GPIO writes).
  2. Only afterwards: `tbp_feed_train_active = 1`, `step_segment_remaining = steps`, and **`step_scheduled_total = c->step_completed + steps`** (`:200`), which becomes the command's `tbp_feed_target`.
- If the ISR fires between `step_active = 1` and the read of `step_completed`, it advances one phase (`step_completed` 0→1). The target becomes `1 + 80 = 81`, but the train only ever performs 80 transitions.
- The train ends (`step_active = 0`, `step_remaining = 0`, `step_completed = 80`). The MOVE completion test in `pb_tbp_exec_poll` (`pb_core.h` ~`:915`, `:1134` for ROW) is `step_completed < tbp_feed_target`, which is true forever, so the command emits `PROGRESS remaining=0` every second and never terminates. Everything behind it in the FIFO is stuck.
- **Why only 1000 µs:** the first tick after start does real work only when `step_wait_ms == 1`. For 2000 µs or more the first tick merely decrements the wait counter, so an ISR in the window is harmless. In the simulation (section 5) injecting one tick into the window wedges the 1000 µs case and leaves 2000 µs and 6000 µs correct.
- The same helper is used by the ROW path (`pb_core.h:1040`, `:1120`), so 1000 µs ROWs are exposed too. The "extend an active train" branches of `pb_step_feed_schedule` also read/modify `step_*` fields that the ISR mutates, with no interrupt protection; they should be reviewed for the same class of race.

Confidence: the mechanism is reproduced deterministically in a model that uses the real `pb_core.h`. It matches the hardware signature exactly (same progress frame bytes). Whether this is the *only* wedge trigger on hardware is not proven.

### 3.2 Secondary: the legacy 5 s watchdog applies to TBP sessions and is not fed by progress (firmware)

- `pb_init` sets `c->wdg = 5000` (`pb_core.h:339`). `pb_poll` trips it at `pb_core.h:1675` when any non-PA1/PC7 pin is an output and `now - last_feed >= wdg`.
- `last_feed` is refreshed by a valid inbound TBP frame (`pb_core.h:1212`), by `pb_step_tick` while `step_active` (`pb_printer.h:~398`), and by a few command paths. It is **not** refreshed by the firmware's own progress/queued status frames.
- On trip it calls `pb_tbp_cancel_work(c, 3u)` (CANCELLED reason 03), `pb_safe_state`, sets `tbp_active = 0` (so the next command gets `ERR NOSESSION`), and prints the raw line `!wdg`, which is not a TBP frame.
- Consequence: any TBP job that legitimately takes more than 5 s while the host is silent (long DWELL, long MOVE, a wedged job) gets its session killed. This turns a stuck job into a full session loss and hides the original fault.

### 3.3 Contributing: host sends no keepalive while commands are pending (Bun host)

- `host/pinbridge/bun/server.ts:92-95`: the 1.5 s STAT keepalive returns early when `this.pending.size > 0`. With a full window (`maxWindow` = 4) it also could not send one: credits are exhausted.
- Host retry timers are reset by every PROGRESS/QUEUED status the firmware sends each second, so the host never retransmits either. The firmware therefore sees no inbound frame for >5 s.
- Fixing only the host is not enough (the window can be full), so 3.2 should be fixed in firmware; a host change is defense in depth.

### 3.4 Diagnostics gap (already addressed)

The Bun host dropped non-TBP lines as `Ignored non-TBP UART line` without content. `8a1272e` adds raw TX/RX/`RX?`/`DROP` logging behind `--debug` / `PINBRIDGE_DEBUG=1`. The UI Event stream still shows only the generic message.

## 4. Open questions

1. **Unexplained cancel at 10:51:42** (see 2.2): a freshly ACKed command was cancelled with reason 03 and `!wdg`-like non-TBP output within the same second, with no wedged job and STAT polling healthy. Candidates to check: other callers of reason 03 (`pb_core.h:779`, an unread failure path), `last_feed`/`pb_now` arithmetic (`hal_ms` is `jiffies * 10`, 10 ms resolution), a MOVE issued right after `pb_coils_idle`/`pb_motor_outputs_release`. Re-test with `--debug` to capture it.
2. **USB replug requirement:** the logs show `No UART device found` for ~21 s after one failure (device node gone), but a later software reconnect worked immediately. It is unknown whether the MCU reset, USB detached (`DEVICE_EVENT_OUT` sets `detached` and `request_fast_abort`), or the owner replugged. Check whether anything leaves `estop_fast_requested` stuck after a detach (`app.c:20,59,114,329`): while it is set, `pb_step_timer` clears `step_active`/`step_done_pending` every tick, which would also wedge a MOVE.
3. **Which firmware is flashed.** `build_id` is 0. Consider embedding the git SHA so a trace can be tied to a commit.

## 5. Reproduction (host-side simulation, no hardware)

The existing C transcript harness calls `pb_step_tick` synchronously between commands, so it cannot expose this. The harness below injects one ISR tick inside the first motor-phase write after `step_active` becomes 1.

Build and run from the repo root (compiles `pb_core.h` on the host; this is not a firmware build):

```sh
# save the code block below as /tmp/race.c first
cc -std=gnu11 -O1 -I firmware/apps/pinbridge/src -o /tmp/race /tmp/race.c
/tmp/race
```

Expected current output: `inject=1` at 1000 µs prints `done=0 … completed=80 remaining=0 scheduled_total=81 feed_target=81`, a repeating `0282050600000000…` progress frame, then `…0282 02 05 03` (cancel 03) and `!wdg` after ~5 s of silence. 2000 µs and 6000 µs with `inject=1` complete normally. After the fix every case must complete.

```c
#define PB_PINS_HEADER "pb_pins.h"
#include <stdio.h>
#include <string.h>
#include "pb_core.h"
static unsigned now_ms; static uint16_t outs[4], pads[4]; static pb_core *C;
static int inject_armed, injected;
static char out[65536]; static size_t used;
static unsigned PER = 1000;
static void maybe_isr(void){ if(inject_armed && C && C->step_active){ inject_armed=0; injected=1; pb_step_tick(C);} }
static int mode(void*x,unsigned p,unsigned b,unsigned m,unsigned l){(void)x;(void)m; if(l)outs[p]|=1u<<b; else outs[p]&=~(1u<<b); maybe_isr(); return 0;}
static void wr(void*x,unsigned p,uint16_t mask,uint16_t v){(void)x; outs[p]=(outs[p]&~mask)|(v&mask); maybe_isr();}
static uint16_t rd(void*x,unsigned p){(void)x;return pads[p]|outs[p];}
static int adc(void*x,int ch,unsigned*r,unsigned*m){(void)x; if(r)*r=ch==-2?500:412; if(m)*m=ch==-2?4000:ch==10?1236:1219; return 0;}
static uint32_t ms(void*x){(void)x;return now_ms;}
static void dly(void*x,unsigned us){(void)x;(void)us;}
static int tx(void*x,const char*s,size_t n){(void)x; if(used+n<sizeof out-1){memcpy(out+used,s,n);used+=n;out[used]=0;} return 1;}
static void send(pb_core*c,uint8_t id,uint8_t op,const uint8_t*d,uint8_t len){
  uint8_t raw[64]; char hex[140]; int i; raw[0]=id;raw[1]=op;raw[2]=len; memcpy(raw+3,d,len);
  uint16_t crc=pb_tbp_crc(raw,3+len); raw[3+len]=crc>>8; raw[4+len]=crc;
  for(i=0;i<5+len;i++) sprintf(hex+2*i,"%02X",raw[i]); strcat(hex,"\n");
  pb_rx(c,(const unsigned char*)hex,strlen(hex));
}
static void run(pb_core*c,unsigned n){ while(n--){ now_ms++; pb_step_tick(c); if(now_ms%10==0) pb_poll(c);} }
static int trial(int inject){
  static pb_core c; pb_hal h={0,mode,wr,rd,adc,0,ms,tx,dly,wr};
  memset(&c,0,sizeof c); memset(outs,0,sizeof outs); used=0; out[0]=0; now_ms=1000; injected=0; inject_armed=0;
  pb_init(&c,&h); C=&c;
  { uint8_t d[1]={1}; send(&c,1,0x01,d,1); }
  run(&c,50); used=0; out[0]=0;
  { uint8_t d[6]={0,0,0,80,(uint8_t)(PER>>8),(uint8_t)PER}; inject_armed=inject; send(&c,2,0x11,d,6); }
  run(&c,2500);
  if(!strstr(out,"028204")) run(&c,6000);      /* DONE frame for id 2 starts 028204 */
  int done = strstr(out,"028204")!=0;
  printf("period=%uus inject=%d injected=%d done=%d step_active=%u completed=%u remaining=%u scheduled_total=%u feed_target=%u\n",
    PER,inject,injected,done,c.step_active,(unsigned)c.step_completed,(unsigned)c.step_remaining,
    (unsigned)c.step_scheduled_total,(unsigned)c.tbp_feed_target);
  printf("  tx tail: %.200s\n", out);
  return done;
}
int main(void){ PER=1000; trial(0); trial(1); PER=2000; trial(1); PER=6000; trial(1); return 0; }
```

(The harness is embedded here rather than committed as a test. A fix should add a proper regression test; see 6.)

## 6. Proposed fix plan (separate commits; none started)

1. **Firmware – wedge (3.1).** Capture the base `step_completed` before starting the train, or fully initialize train state (`tbp_feed_train_active`, `step_segment_remaining`, scheduled total) before `step_active = 1` / enabling the ISR path, or briefly mask the step ISR around start/extend. Review the extend branches of `pb_step_feed_schedule` for the same race. Add a defensive completion path: if `!step_active && step_remaining == 0 && step_completed < tbp_feed_target` (train ended but target unreachable), terminate the command (DONE or FAILED with a defined reason) instead of waiting forever.
2. **Firmware – watchdog (3.2).** Do not let the legacy 5 s watchdog act on an active TBP session with queued/running work (or refresh `last_feed` when sending progress). Report a watchdog trip as a TBP event or a defined cancel reason instead of a raw `!wdg` line, and update the spec/matrix accordingly.
3. **Host (3.3/3.4).** Surface raw non-TBP lines in the UI log (at least in the Event stream). Consider a keepalive that works with a full window, or reserve one credit for STAT.
4. **Defaults (symptom 1).** Change the 6000 µs defaults to 1000 µs only **after** fix 1 lands: `PB_TUNE_STEPPER_DEFAULT_PERIOD_US` and `PB_TUNE_BOOT_WIGGLE_PERIOD_US` in `pb_tuning.h`, `?? 6000` in `server.ts` (`row`, `print-solid`), and any UI default in `public/app.js`. Note 1000 µs shrinks the queued-row buffer to ~8 ms (Sprint 6 known issue 3), so coil release between rows becomes more likely; decide on the idle-hold policy together with this.
5. **Tests.** Add a C regression test with an ISR-injection hook (like the harness above) that fails today and passes after fix 1, covering MOVE and ROW at 1000 µs. Add a transcript or unit test for the watchdog-vs-TBP behavior.
6. **Docs.** Update the Sprint 4/6 handoff with the commits, checks and the evidence boundaries (CI/simulation vs hardware), add a `docs/journal.md` entry, and add this bug's resolution here.

## 7. Acceptance criteria

- The ISR-injection test wedges at 1000 µs on `da115f8` and completes at 1000, 2000 and 6000 µs after the fix, for MOVE and ROW, including back-to-back forward/reverse sequences.
- A TBP job running longer than 5 s with a silent host is not cancelled by the legacy watchdog; a genuinely lost host still stops motion/heater via the TBP link timeout.
- No raw `!wdg` text on the wire during a TBP session.
- `bun run check`, Python unittests, `./scripts/check-pinbridge.sh`, `git diff --check`, all C transcripts (both pin headers), and the CI run for the pushed commit pass.
- Hardware confirmation (owner-run): repeated feed/retract at 1000 µs for several minutes without a stall, using `--debug` traces. This must be reported separately from CI evidence.

## 8. Constraints for whoever fixes this

- Read `~/AGENTS.md` and `docs/sprints/README.md` first. No local firmware builds (CI only), no flashing or hardware writes without explicit owner approval, keep each commit to one behavior, never print secrets.
- Do not claim hardware behavior from compile or simulation results.
- Local checks that are allowed: Bun/Python host tests and the host-side C transcript build (`cc -std=gnu11 -O1 -Wall -I firmware/apps/pinbridge/tests/c -o /tmp/pb_test firmware/apps/pinbridge/tests/c/pb_test.c`, and a second build with `-I firmware/apps/pinbridge/src -DPB_PINS_HEADER='"pb_pins.h"'` as `pb_test_real`; each transcript must pass with at least one of the two). Never run `cargo test` on this box.
