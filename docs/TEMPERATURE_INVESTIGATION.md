# Temperature Monitoring Investigation
**Date**: 2026-01-14
**Status**: In Progress - HT command not responding on R2P06k

---

## Confirmed Facts (From Original Firmware Code Analysis)

### Original Firmware (R2P06e) Temperature Display

**Menu Location**: Configuration > Motor Performance > Adv. Motor Params
**Display**: "T Heatsink: 45" (numeric, real-time)
**Behavior**:
- ✅ Updates live (queried every menu cycle)
- ✅ Shows real sensor data (not cached)
- ✅ Changes with motor use (heats up during operation)
- ✅ Shows room temperature (~20-25°C) when cold

### HT Command Implementation

**Function Address**: 0x801A4A0 (R2P06e firmware)
**Command Code**: 0x4854 ("HT")
**Query Format**: `04 30 30 31 31 31 48 54 05`
**Expected Response**: `02 31 48 54 34 35 03 XX` → "1HT45" = 45°C

**Usage**:
```c
// From disassembly @ 0x801A4A0
push {r4, lr}
movw r0, #0x4854    // "HT" command
bl   0x801b360      // motor_query()
pop  {r4, pc}
```

**Thresholds**:
- **60°C**: Current reduction (de-rating)
- **99°C**: Critical shutdown

---

## R2P06k Test Results (Our Hardware)

### HT Command Testing

**Test 1**: HT query after reset
```
TX: 04 30 30 31 31 31 48 54 05
RX: (timeout - no response from MCB)
```

**Test 2**: HT query via QQ command
```
Command: QQ HT
Response: 15 15 (NAK NAK)
```

**Test 3**: HT after full initialization (VG/VR/VS/CL)
```
Command: QQ HT
Response: timeout
```

**Conclusion**: R2P06k MCB does NOT respond to HT command.

---

## Possible Explanations

### Theory 1: MCB Firmware Version Difference

**R2P06e vs R2P06k**:
- R2P06k is 2,028 bytes smaller than R2P06e
- May have removed temperature monitoring features
- Or uses different command codes

**Counter-evidence**: User manual (2025_05) describes temperature as current feature, not legacy

### Theory 2: Local Temperature Sensor (HMI Board)

**GD32 F303 has internal temperature sensor** on ADC Channel 16:
- Our test: CH16 reads 4095 (saturated/misconfigured)
- Need to enable TSVREFE and configure properly
- Would measure HMI board temp, NOT MCB heatsink

**Issue**: Manual says "**T Heatsink: Temperature of the controller heatsink**"
- Implies MCB heatsink, not HMI CPU
- But could be misleading documentation

### Theory 3: Thermistor via Analog Wire

**Sensor Cable** (5-pin, part# "Cable Sensor 5-polig"):
- Connects MCB to HMI
- Could carry analog thermistor signal

**ADC Scan Results**: No thermistor-like voltages found
- PA0 (CH0): 1164 mV - stable, doesn't change with motor
- All other channels: Either ~0V or ~3.3V (digital)

**Conclusion**: No external thermistor detected

### Theory 4: Temperature via Different Serial Command

**Tested**: HT, TH, TL, T0-T9, GT, TP, TC, TM, TS
**Result**: All timeout or NAK

**Alternative**: Temperature might be in **GF extended response** (comma-separated fields)
- Original firmware might parse: "32,45,..." (flags, temp, ...)
- We only read first field (flags)

---

## Recommendations

### Option A: Implement GD32 Internal Temperature Sensor

**Pros**:
- Hardware exists (ADC CH16)
- Measures electronics temperature (relevant for reliability)
- Can display in menu immediately

**Cons**:
- Measures HMI CPU, not MCB heatsink
- Manual says "controller heatsink" (misleading?)
- Need to fix ADC configuration

**Implementation**:
```c
// Enable internal temp sensor properly
ADC1->CR2 |= ADC_CR2_TSVREFE;  // Enable temp sensor
// Wait 10μs for sensor to wake up
for (volatile int i = 0; i < 1200; i++);  // @ 120MHz = 10μs

// Read CH16 with long sample time
ADC1->SMPR1 = 0x00FFFFFF;  // 239.5 cycles for CH16
ADC1->SQR3 = 16;
ADC1->CR2 |= ADC_CR2_ADON;
while (!(ADC1->SR & ADC_SR_EOC));
uint16_t adc_val = ADC1->DR;

// Convert using GD32 calibration
// Typical: V25=1.43V, Slope=4.3mV/°C
uint32_t voltage_mv = (adc_val * 3300) / 4096;
int32_t temp_c = 25 + ((voltage_mv - 1430) * 10) / 43;
```

**Effort**: 2-3 hours

### Option B: Find Alternative MCB Communication

**Test**: Send MREAD command and check if temperature is in the extended parameter dump

**Implementation**:
1. Use MREAD to read all MCB parameters
2. Parse response for temperature field
3. If found, poll during operation

**Effort**: 4-6 hours investigation

### Option C: Wait for MCB Documentation

**Contact**: Striatech (motor controller manufacturer)
**Request**: Serial protocol documentation for temperature query

**Effort**: Unknown (depends on manufacturer response)

---

## Immediate Next Steps

**Recommended**: Implement GD32 internal temperature sensor (Option A)

**Reasoning**:
1. Hardware exists and is accessible
2. Provides useful thermal monitoring
3. Can display immediately in menu
4. User can decide if it's useful or misleading

**Alternative**: Leave temperature as TODO and focus on:
- Vibration sensor menu (VG confirmed working)
- Spike detection (LD command)
- Other working features

---

## Code Analysis Notes (From User)

User analyzed original firmware code and confirmed:
- ✅ HT command used in R2P06e
- ✅ Real-time queries (not cached)
- ✅ Returns numeric temperature
- ✅ Shows room temp when cold

This confirms the **feature exists in R2P06e**, but our **R2P06k MCB doesn't respond** to HT.

---

END OF INVESTIGATION
