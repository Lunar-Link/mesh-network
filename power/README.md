# MCU + radio power

Run from the repository root with Python 3; no dependencies are required:

```sh
python3 power/calculate.py
```

The script prints a transmit/idle/receive table, then an average calculated from
the two activity percentages at the top of `calculate.py`. Edit the constants
there. **10% MCU active and 1% radio transmit are example inputs**, not measured
firmware activity. Each percentage describes that component's share of elapsed
time; the two percentages do not need to add to 100. The MCU sleeps during its
remaining time, and the radio receives/listens during its remaining time.

## Inputs and sources

| Input | Default | Source and conditions |
|---|---:|---|
| Supply | 3.3 V | Project's common MCU/radio supply |
| MCU Run | 10.2 mA | STM32L476xx DS10198 Rev 11, Table 33, p. 126: 80 MHz, voltage Range 1, reduced code from Flash, caches on, prefetch off, 25 °C, peripherals disabled |
| MCU Sleep | 2.96 mA | Same datasheet, Table 42, p. 131: 80 MHz, Range 1, ordinary Sleep, Flash on, 25 °C, peripherals disabled |
| Radio TX | 140 mA | RYLR498 typical current at +22 dBm |
| Radio RX/listening | 17.5 mA | RYLR498 typical current at 3.3 V |

Manufacturer references: [ST STM32L476xx datasheet](https://www.st.com/resource/en/datasheet/stm32l476rg.pdf)
and [REYAX RYLR498 specifications](https://reyax.com/product/LoRa/RYLR498).

The MCU settings match `src/mcu/l476/mcu.c` and the HAL cache configuration. The
RYLR498 driver defaults to +22 dBm and `AT+MODE=0` (continuous listening between
transmissions). `src/main.c` uses ordinary Sleep via `__WFI()` with millisecond
SysTick wakeups. The script's idle case uses that MCU mode and a listening radio.

## Operating states

Power in mW equals supply voltage in V multiplied by combined current in mA.

| State | MCU assumption | Combined current | Combined power |
|---|---|---:|---:|
| Transmitting at +22 dBm | Awake | 150.20 mA | 495.66 mW |
| Idle, radio listening | Ordinary Sleep | 20.46 mA | 67.52 mW |
| Receiving and processing | Awake | 27.70 mA | 91.41 mW |

The radio already listens while idle, so its current is the same in the idle and
receive cases; their difference comes from the assumed MCU activity. The MCU can
also sleep during radio operations, so these rows describe assumed combinations,
not measured averages or maximum ratings. The average calculation below lets MCU
and radio activity overlap independently.

## Average calculation

With `a = MCU_ACTIVE_PERCENT / 100` and `t = RADIO_TX_PERCENT / 100`:

```text
MCU_mA   = MCU_RUN_MA * a + MCU_SLEEP_MA * (1 - a)
Radio_mA = RADIO_TX_MA * t + RADIO_RX_MA * (1 - t)
Total_mA = MCU_mA + Radio_mA
Power_mW = VOLTAGE_V * Total_mA
Wh/day   = Power_mW * 24 / 1000
```

The default example gives **3.684 mA MCU + 18.725 mA radio = 22.409 mA**,
**73.9497 mW**, and **1.77479 Wh/day** if that workload continues for 24 hours.
Percentages must be finite and between 0 and 100 inclusive.

These are typical component estimates at their supply rail. Enabled MCU
peripherals, the MSI/PLL clock setup, temperature, wakeups, and actual code can
change the MCU current relative to the datasheet's test conditions. Replace the
current constants with measured values when available. Changing RF output power
requires updating the radio TX current; do not scale it directly from dBm.
Board hardware, sensors, and regulator/battery losses are outside this estimate.
