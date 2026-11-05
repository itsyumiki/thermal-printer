// One-line, human-readable descriptions of commands and printer messages, for logs and consoles.
// Output only: nothing here is ever parsed back, the wire protocol is hex frames and nothing else.
// Pure functions, no I/O.
import {
  CAPS,
  CONFIG_KEY,
  PRINTER_STATE,
  ROW_FLAG,
  STAT_FLAG,
  TIME_SRC,
  flagNames,
  nameOf,
} from "./protocol.ts";
import { toHex } from "./frame.ts";
import { commandPayload, type Command } from "./commands.ts";
import {
  decodeMessage,
  type DecodeContext,
  type Info,
  type Message,
  type Stat,
} from "./messages.ts";

const hex2 = (n: number) => n.toString(16).padStart(2, "0");

/** "move steps=200 period=3000us" */
export function describeCommand(cmd: Command): string {
  switch (cmd.type) {
    case "hello":
      return `hello v${cmd.version ?? 1}`;
    case "stat":
    case "flush":
    case "estop":
      return cmd.type;
    case "config": {
      const name =
        nameOf(CONFIG_KEY, cmd.key)?.toLowerCase() ?? `key ${cmd.key}`;
      return `config ${name}=${cmd.value}`;
    }
    case "row": {
      const mask =
        cmd.mask === "black" || cmd.mask === "white"
          ? cmd.mask
          : `${typeof cmd.mask === "string" ? cmd.mask.length / 2 : cmd.mask.length}B mask`;
      return `row ${mask} burn=${cmd.burnUs}us steps=${cmd.steps} period=${cmd.periodUs}us slices=${cmd.slices ?? 1}`;
    }
    case "move":
      return `move steps=${cmd.steps} period=${cmd.periodUs}us`;
    case "dwell":
      return `dwell ${cmd.ms}ms`;
    case "coils":
      return `coils ${cmd.on ? "on" : "off"}`;
    case "timetest":
      return `timetest ${typeof cmd.src === "number" ? (nameOf(TIME_SRC, cmd.src) ?? cmd.src) : cmd.src} ${cmd.ms}ms`;
  }
}

export function describeStat(s: Stat): string {
  const f = flagNames(STAT_FLAG, s.flags);
  const na = (v: number, unit: string) =>
    v === 0xffff ? "n/a" : `${v}${unit}`;
  const paper =
    s.paper === 1 ? "present" : s.paper === 0 ? "absent" : "unknown";
  return (
    `${nameOf(PRINTER_STATE, s.state)?.toLowerCase() ?? `state ${s.state}`} · queue ${s.queueUsed}/${s.queueDepth}` +
    ` · running ${s.runningId || "-"} · last done ${s.lastTerminalId || "-"}` +
    ` · head raw ${na(s.headTempRaw, "")} · batt ${na(s.batteryMv, " mV")} (raw ${na(s.batteryRaw, "")})` +
    ` · paper ${paper}${f.length ? ` · ${f.join("+").toLowerCase()}` : ""}`
  );
}

export function describeInfo(i: Info): string {
  const caps = flagNames(CAPS, i.caps).join("+").toLowerCase() || "none";
  return (
    `TBP v${i.version} · build ${i.buildId.toString(16).padStart(8, "0")} · uid ${i.uid} · ${i.dots} dots` +
    ` · queue ${i.queueDepth}/window ${i.maxWindow} · min period ${i.minPeriodUs}us (tick ${i.stepTickUs}us)` +
    ` · burn max ${i.burnMaxUs}us (tick ${i.burnTickUs}us, guard ${i.guardUs}us)` +
    ` · duty ${i.dutyMaxUs}us/${i.dutyWindowMs}ms · shift ${i.shiftUs}us · caps ${caps}`
  );
}

/** One line for any printer message, e.g. `DONE id=5 row: burn 1500us period 6000us flags=cold_start`. */
export function describeMessage(m: Message): string {
  switch (m.type) {
    case "ack":
      return `ACK id=${m.id} queue_used=${m.queueUsed}`;
    case "err":
      return `ERR id=${m.id} ${m.codeName} (0x${hex2(m.code)}) arg=${m.arg}`;
    case "event":
      switch (m.event) {
        case "heartbeat":
          return `EVENT heartbeat: ${describeStat(m.stat)}`;
        case "paper":
          return `EVENT paper ${m.present === 1 ? "present" : m.present === 0 ? "absent" : "unknown"}`;
        case "hot":
          return `EVENT head ${m.active ? "too hot, waiting" : "cooled"}`;
        case "reset":
          return `EVENT reset reason=${m.reason === 1 ? "power-on" : m.reason === 2 ? "watchdog" : "other"} (session gone)`;
        case "stopped":
          return `EVENT stopped reason=${m.reason}`;
        default:
          return `EVENT 0x${hex2(m.eventByte)} data=${toHex(m.data)}`;
      }
    case "status":
      switch (m.state) {
        case "queued":
          return `QUEUED id=${m.id} pos=${m.pos} queue_used=${m.queueUsed}`;
        case "running":
          return `RUNNING id=${m.id}`;
        case "progress":
          return `PROGRESS id=${m.id} remaining=${m.remaining}`;
        case "failed":
          return `FAILED id=${m.id} ${m.codeName} (0x${hex2(m.code)}) arg=${m.arg}`;
        case "cancelled":
          return `CANCELLED id=${m.id} reason=${m.reasonName.toLowerCase()}`;
        case "done": {
          const r = m.result;
          switch (r.kind) {
            case "info":
              return `DONE id=${m.id} hello: ${describeInfo(r.info)}`;
            case "stat":
              return `DONE id=${m.id} stat: ${describeStat(r.stat)}`;
            case "row": {
              const f = flagNames(ROW_FLAG, r.row.flags)
                .join("+")
                .toLowerCase();
              return (
                `DONE id=${m.id} row: burn ${r.row.burnEffUs}us period ${r.row.periodEffUs}us` +
                ` head raw ${r.row.headTempRaw === 0xffff ? "n/a" : r.row.headTempRaw}` +
                ` batt ${r.row.batteryMv === 0xffff ? "n/a" : r.row.batteryMv + " mV"}${f ? ` flags=${f}` : ""}`
              );
            }
            case "flushed":
              return `DONE id=${m.id} flushed=${r.flushed}`;
            case "move":
              return `DONE id=${m.id} move: period ${r.periodEffUs}us`;
            case "config":
              return `DONE id=${m.id} config: key ${r.key} = ${r.value}`;
            case "timetest": {
              const t = r.test;
              return (
                `DONE id=${m.id} timetest ${nameOf(TIME_SRC, t.src) ?? t.src}: n=${t.n} want=${t.wantUs}us dj=${t.dj}` +
                (t.div ? ` ticks=${t.ticks} div=${t.div} isr=${t.isr}` : "")
              );
            }
            case "none":
              return `DONE id=${m.id}`;
            default:
              return `DONE id=${m.id} data=${toHex(r.data)}`;
          }
        }
        default:
          return `STATUS id=${m.id} state=0x${hex2(m.stateByte)} data=${toHex(m.data)}`;
      }
    default:
      return `FRAME id=${m.id} op=0x${hex2(m.op)} data=${toHex(m.data)}`;
  }
}

/** A raw line from the wire -> `<line>  [description]`; text that is not a valid TBP frame is returned as it came. */
export function describeLine(line: string, ctx?: DecodeContext): string {
  const text = line.trim();
  try {
    return `${text}  [${describeMessage(decodeMessage(text, ctx))}]`;
  } catch {
    return text;
  }
}

/** The hex line that `cmd` would send with id `id`, plus its description; handy for dry runs. */
export function describeSend(cmd: Command, id: number): string {
  const { op, data } = commandPayload(cmd);
  return `id=${id} op=0x${hex2(op)} data=${toHex(data) || "-"}  [${describeCommand(cmd)}]`;
}
