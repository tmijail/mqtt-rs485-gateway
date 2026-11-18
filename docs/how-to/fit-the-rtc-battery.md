# Fit the RTC battery

This procedure tells how to install the RTC battery of the gateway and how to make sure that the RTC keeps the time. Do this procedure before you install the gateway. The box of the board does not contain a cell. Without the cell, the gateway sends no data after a power cut until SNTP or an operator sets the time.

- For the facts about the clock, the cell and the signals of the gateway, refer to [The clock and the RTC battery](../reference/clock-and-rtc-battery.md).
- For the reasons, refer to [No session without a valid clock](../reference/sparkplug-behavior.md#no-session-without-a-valid-clock-time-1-160).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The real I²C access to the RTC is compile-only. The data of the holder comes from the Waveshare product page. The board tests of #126 will measure the holder and correct this document.

## Before you start

You need:

- One rechargeable 3 V coin cell of size 1220 (12 mm diameter, 2 mm thickness), for example an ML1220.
- Access to the board with the power off.
- Access to the web UI of the gateway, or a computer with a USB-C cable for the shell.

> **CAUTION: Do not install a CR1220 cell.** The board charges the cell. A primary (not rechargeable) cell can be damaged.

## Procedure

1. Disconnect all power from the board: the terminal block, PoE and USB-C.
2. Find the holder of the 1220 coin cell on the board.
3. Put the cell in the holder. Obey the polarity mark of the holder.
4. Connect the power.
5. Make sure that the gateway gets a valid time:
   - If the gateway can reach its SNTP server, wait for the synchronization.
   - If not, open the web UI. On the Status tab, click "Set the time from the browser".
6. Make sure that the web UI shows no banner "Waiting for a valid clock". The gateway writes the time into the RTC.

## Make sure that the RTC keeps the time

1. Disconnect all power from the board for 1 min.
2. Connect the power, but do not let the gateway reach its SNTP server. For example, disconnect the network cable.
3. Type `status` on the shell. Make sure that the `time` line shows `from the RTC (not synchronised); RTC ok`.
4. Make sure that the shell shows no `battery` line.
5. Connect the network cable again.

If the test fails, refer to [If the gateway tells you to examine the battery](#if-the-gateway-tells-you-to-examine-the-battery).

## If the gateway tells you to examine the battery

The gateway tells this when the RTC lost the time while the power was off (the OS bit of the PCF85063 was set at boot). The web UI shows it in a banner, the shell in the `battery` line of `status`, and the log in a line of `io` at boot.

1. Make sure that the holder contains a cell.
2. Make sure that the cell is a rechargeable 3 V cell, not a CR1220.
3. Make sure that the cell is in the holder with the correct polarity.
4. Remove the cell, and measure its voltage. If it is much less than 3 V, replace it.
5. Do the procedure again.

The board charges the cell while the power is on. A new cell can have a low charge. Then the RTC can lose the time at the first power cut. Nobody measured the charge time yet (#126).
