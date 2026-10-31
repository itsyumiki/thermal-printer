# Troubleshooting

| Failure | Cause | Fix |
|---|---|---|
| `lsusb` missing `4c4a:2342` | Printer not in UBOOT mode | Power-cycle into UBOOT mode. |
| `ERR_FAILED` in `isd_download` | Vendor tool expects an unsupported environment | Use `jluboottool.py` instead (it is already working). |
| `jluboottool.py` timeout | Session timed out | Power-cycle or use RP2040 key sequence to restart UBOOT session. |
| `ModuleNotFoundError: No module named 'yaml'` | Venv not used / deps missing | `~/jlvenv/bin/pip install pyyaml ...` |
| `patch_app.py` mismatch errors | Wrong key / dump not a match | Check chip key (`info` from `fm1t.py`), use the correct dump hash. |
| Printer doesn't boot (after flash) | Incorrect oscillator/power latch config | The demo SDK's board settings don't match the printer's board. Needs investigation (see [`journal.md`](journal.md)). |
| `xxd: not found` | Tools missing on build host | `sudo apt-get install xxd` |
