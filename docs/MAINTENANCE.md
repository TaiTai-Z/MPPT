# Maintenance rules

## Source of truth

The maintained controller source is the v2.3.0 tree under `firmware/`. Do not
edit generated files under `Out/`; rebuild them from the Keil project instead.
The netlists in `firmware/STM32G474RBT3_RTThread_Safe_v2.3.0/hardware/netlists/`
are reference inputs for the current board profile, not generated build files.

## Required record for each change

Append one dated section to `docs/logs/remade_master.md` containing:

1. observed symptom or design requirement;
2. exact source files and behavior changed;
3. static checks and build result;
4. flash result, if programming was performed;
5. bench-test command, measurement and remaining risk.

Keep raw command output in `docs/build-logs/` when it is useful for traceability.
Do not claim PWM or protection validation from compilation alone.

## Verification order

1. Run `tools/check_project.bat`.
2. Run `tools/build_keil.bat` and require zero errors and zero warnings.
3. Review the map file locally when timing, SRAM or interrupt ownership changes.
4. Flash only the verified image with `tools/flash_keil.bat`.
5. After manual reset, query `HRTIMDIAG` and `STATUS` before any `START` command.
6. For power tests, begin with current limiting and record oscilloscope evidence
   for PWM frequency, duty, ADC synchronization and each protection trip.
