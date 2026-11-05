// Console on stdin: paste a TBP hex frame, see the printer's reply. Hex only, no plaintext commands.
// Needed because the UART is owned by this process, so `tio` and friends cannot open it at the same time.
import { createInterface } from "node:readline";

export type ConsoleTarget = { sendHex(line: string): Promise<string[]> };

/**
 * Reads lines from `input` and runs them one at a time through `target.sendHex`, printing each result
 * line (or the error, prefixed with "!") through `print`. Blank lines and lines starting with # are skipped.
 * Returns a function that stops listening. EOF just stops the console; it never ends the host.
 */
export function startConsole(
  target: ConsoleTarget,
  input: NodeJS.ReadableStream,
  print: (line: string) => void = (l) => console.log(l),
  onEnd: () => void = () => {},
): () => void {
  // terminal:false keeps Ctrl-C going to the process instead of being swallowed by readline.
  const rl = createInterface({ input, terminal: false });
  let chain: Promise<void> = Promise.resolve();
  rl.on("line", (raw) => {
    const line = raw.trim();
    if (!line || line.startsWith("#")) return;
    chain = chain.then(async () => {
      try {
        for (const out of await target.sendHex(line)) print(out);
      } catch (e) {
        print(`! ${e instanceof Error ? e.message : String(e)}`);
      }
    });
  });
  rl.on("close", () => {
    void chain.then(onEnd);
  });
  return () => rl.close();
}
