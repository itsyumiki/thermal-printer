import { describe, expect, test } from "bun:test";
import { Uart } from "./uart.ts";

describe("UART partial-line recovery", () => {
  test("discards only the incomplete buffered line", () => {
    const uart = new Uart("/dev/not-opened");
    (uart as any).buf = "partial TBP frame";
    uart.discardPartialLine();
    expect((uart as any).buf).toBe("");
    expect(uart.isOpen).toBe(false);
  });
});
