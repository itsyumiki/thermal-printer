"""jlpin v2 line protocol helpers."""


class PinbridgeError(Exception):
    pass


class ProtocolError(PinbridgeError):
    pass


class BoardReset(PinbridgeError):
    pass


class PinError(PinbridgeError):
    pass


class CapabilityError(PinbridgeError):
    pass


class ModeError(PinbridgeError):
    pass


class ArgumentError(PinbridgeError):
    pass


class BusyError(PinbridgeError):
    pass


class LineLengthError(PinbridgeError):
    pass


ERRORS = {
    "pin": PinError,
    "cap": CapabilityError,
    "mod": ModeError,
    "arg": ArgumentError,
    "bsy": BusyError,
    "len": LineLengthError,
}


def parse_reply(line: str) -> list[str]:
    line = line.strip("\r\n")
    if line.startswith("err "):
        bits = line.split(maxsplit=2)
        cls = ERRORS.get(bits[1], ProtocolError)
        raise cls(bits[2] if len(bits) > 2 else bits[1])
    if not line.startswith("ok"):
        raise ProtocolError(f"unexpected reply: {line!r}")
    return line.split()[1:]
