// One call, both directions: a hex line in, a typed and described result out.
// Hex frames are the only thing TBP speaks, so this is the only entry point a script needs.
// Pure functions, no I/O.
import { decode, type Frame } from "./frame.ts";
import { KIND } from "./protocol.ts";
import { decodeCommand, type Command } from "./commands.ts";
import { decodeMessage, type DecodeContext, type Message } from "./messages.ts";
import { describeCommand, describeMessage } from "./describe.ts";

export type Translation =
  /** A host -> printer frame (HELLO, STAT, ROW, ...). */
  | {
      ok: true;
      direction: "to-printer";
      hex: string;
      frame: Frame;
      id: number;
      command: Command;
      text: string;
    }
  /** A printer -> host frame (ACK, STATUS, ERR, EVENT). */
  | {
      ok: true;
      direction: "from-printer";
      hex: string;
      frame: Frame;
      message: Message;
      text: string;
    }
  /** Not a frame we can read. `frame` is set when the framing and CRC were fine but the content was not understood. */
  | { ok: false; hex: string; frame?: Frame; error: string; text: string };

const printerKinds = new Set<number>(Object.values(KIND));

/**
 * Translate one hex line (either direction, either hex case, LF/CRLF tolerated) into its typed meaning.
 * Never throws: a bad line comes back as `{ ok: false, error }`.
 *
 * `ctx.opOf` is optional: tell it which opcode a command id was sent with and DONE bodies are decoded
 * by that opcode instead of being inferred from their length.
 */
export function translate(line: string, ctx?: DecodeContext): Translation {
  const hex = line.trim().toUpperCase();
  let frame: Frame;
  try {
    frame = decode(hex);
  } catch (e) {
    const error = e instanceof Error ? e.message : String(e);
    return { ok: false, hex, error, text: `invalid: ${error}` };
  }
  try {
    if (printerKinds.has(frame.op)) {
      const message = decodeMessage(frame, ctx);
      return {
        ok: true,
        direction: "from-printer",
        hex,
        frame,
        message,
        text: describeMessage(message),
      };
    }
    const { id, command } = decodeCommand(frame);
    return {
      ok: true,
      direction: "to-printer",
      hex,
      frame,
      id,
      command,
      text: describeCommand(command),
    };
  } catch (e) {
    const error = e instanceof Error ? e.message : String(e);
    return { ok: false, hex, frame, error, text: `unrecognised: ${error}` };
  }
}
