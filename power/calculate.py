#!/usr/bin/env python3
"""STM32L476RG + selectable radio estimates. Edit inputs below; sources are in README.md."""

RADIO = "lr1121_chip"
TX_DBM = None          # None uses the selected profile's firmware default.
MCU_VOLTAGE_V = 3.3
MCU_RUN_MA = 10.2       # 80 MHz, caches on, prefetch off; typical, peripherals disabled.
MCU_SLEEP_MA = 2.96     # Ordinary Sleep at 80 MHz, Flash on; typical, peripherals disabled.

# Example workload only: replace with your observed percentages of elapsed time.
MCU_ACTIVE_PERCENT = 10.0
RADIO_TX_PERCENT = 1.0

# TX maps contain published points only. RX also applies while idle/listening.
# Chip profiles exclude TCXO/module overhead; see README.md for test conditions.
RADIOS = {
    "rylr498": {
        "voltage_v": 3.3, "default_dbm": 22, "rx_ma": 17.5,
        "basis": "Module; 433 MHz; detailed TX sweep (summary lists 140 mA at +22 dBm)",
        "tx_ma": {
            0: 41.3, 1: 43.2, 2: 46.0, 3: 47.9, 4: 51.5,
            5: 53.9, 6: 57.1, 7: 61.1, 8: 64.6, 9: 68.6,
            10: 72.7, 11: 77.2, 12: 82.2, 13: 88.5, 14: 94.6,
            15: 101.1, 16: 109.5, 17: 117.7, 18: 125.7,
            19: 131.5, 20: 136.1, 21: 140.0, 22: 144.5,
        },
    },
    "rylr999": {
        "voltage_v": 5.0, "default_dbm": 10, "rx_ma": 15.5,
        "basis": "Module; 915 MHz LoRa; +10 dBm TX current unpublished",
        "tx_ma": {10: None, 30: 950.0},  # Replace None with a measured +10 dBm current.
    },
    "lr1121": {
        "voltage_v": 3.3, "default_dbm": 22, "rx_ma": 9.2,
        "basis": "Waveshare Core1121-LF module; 433 MHz; 125 kHz RX",
        "tx_ma": {22: 115.0},
    },
    "lr1121_2g4": {
        "voltage_v": 3.3, "default_dbm": 13, "rx_ma": 8.1,
        "basis": "Waveshare Core1121 module; 2.4 GHz; 125 kHz RX",
        "tx_ma": {13: 25.0},
    },
    "lr1121_chip": { # what we use currently i believe
        "voltage_v": 3.3, "default_dbm": 22, "rx_ma": 7.8,
        "basis": "Chip only; 434/490 MHz reference; DC-DC; boosted 125 kHz RX; excludes TCXO",
        "tx_ma": {14: 45.0, 17: 70.0, 20: 86.0, 22: 100.0},
    },
    "lr1121_2g4_chip": {
        "voltage_v": 3.3, "default_dbm": 13, "rx_ma": 6.7,
        "basis": "Chip only; 2.4 GHz; DC-DC; boosted 125 kHz RX; excludes TCXO",
        "tx_ma": {10: 23.0, 13: 26.0},
    },
    "lr1121_sband_chip": {
        "voltage_v": 3.3, "default_dbm": 13, "rx_ma": 6.7,
        "basis": "Chip only; 1.9-2.1 GHz S-band; DC-DC; boosted 125 kHz RX; excludes TCXO",
        "tx_ma": {10: 24.0, 13: 28.0},
    },
    "none": {
        "voltage_v": 0.0, "default_dbm": None, "rx_ma": 0.0,
        "basis": "No radio attached; MCU consumption only",
        "tx_ma": {None: 0.0},
    },
}

for name, percent in (("MCU_ACTIVE_PERCENT", MCU_ACTIVE_PERCENT), ("RADIO_TX_PERCENT", RADIO_TX_PERCENT)):
    if not 0 <= percent <= 100:  # Also rejects NaN and infinity.
        raise SystemExit(f"{name} must be finite and between 0 and 100.")

if RADIO not in RADIOS:
    raise SystemExit(f"Unknown RADIO {RADIO!r}. Available keys: {', '.join(RADIOS)}")
radio = RADIOS[RADIO]
tx_dbm = radio["default_dbm"] if TX_DBM is None else TX_DBM
radio_tx_ma = radio["tx_ma"].get(tx_dbm)
if radio_tx_ma is None:
    available = ", ".join(str(dbm) for dbm, current in radio["tx_ma"].items() if current is not None)
    raise SystemExit(
        f"TX current unavailable for {RADIO!r} at TX_DBM={tx_dbm!r}. "
        f"Enter a measured current in RADIOS[{RADIO!r}]['tx_ma'][{tx_dbm!r}], "
        f"or select a published TX_DBM. Available values: {available}"
    )
radio_rx_ma = radio["rx_ma"]
radio_voltage_v = radio["voltage_v"]
tx_label = "n/a (no radio)" if tx_dbm is None else f"{tx_dbm:+g} dBm"
radio_supply = "absent" if RADIO == "none" else f"{radio_voltage_v:g} V"

states = (
    ("Transmit", "Awake", MCU_RUN_MA, radio_tx_ma),
    ("Idle/listening", "Sleep", MCU_SLEEP_MA, radio_rx_ma),
    ("Receive/process", "Awake", MCU_RUN_MA, radio_rx_ma),
)
print(f"Profile: {RADIO} | TX: {tx_label} | {radio['basis']}")
print(f"Supply rails: STM32L476RG at 80 MHz, {MCU_VOLTAGE_V:g} V; radio {radio_supply}")
print(f"{'State':<18} {'MCU':<7} {'MCU (mA)':>10} {'Radio (mA)':>11} {'Total (mW)':>12}")
for state, mcu, mcu_ma, radio_ma in states:
    state_power_mw = MCU_VOLTAGE_V * mcu_ma + radio_voltage_v * radio_ma
    print(f"{state:<18} {mcu:<7} {mcu_ma:10.2f} {radio_ma:11.2f} {state_power_mw:12.2f}")
print("Idle assumes MCU Sleep; TX/RX assume an awake MCU. The MCU can also sleep during TX/RX.")

active = MCU_ACTIVE_PERCENT / 100
tx = RADIO_TX_PERCENT / 100
mcu_average_ma = MCU_RUN_MA * active + MCU_SLEEP_MA * (1 - active)
radio_average_ma = radio_tx_ma * tx + radio_rx_ma * (1 - tx)
power_mw = MCU_VOLTAGE_V * mcu_average_ma + radio_voltage_v * radio_average_ma
energy_wh_per_day = power_mw * 24 / 1000

print(f"\nAverage for editable workload (defaults are examples): MCU active {MCU_ACTIVE_PERCENT:g}%, radio TX {RADIO_TX_PERCENT:g}%")
print(f"MCU:         {mcu_average_ma:.3f} mA at {MCU_VOLTAGE_V:g} V")
print(f"Radio:       {radio_average_ma:.3f} mA ({radio_supply})")
if MCU_VOLTAGE_V == radio_voltage_v:
    print(f"Total:       {mcu_average_ma + radio_average_ma:.3f} mA at {MCU_VOLTAGE_V:g} V")
print(f"Power:       {power_mw:.4f} mW")
print(f"Daily energy: {energy_wh_per_day:.5f} Wh/day")
print("Datasheet baselines; enabled peripherals and wakeups affect actual usage. See README.md.")
