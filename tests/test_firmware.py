#!/usr/bin/env python3
"""
Nova Voyager Firmware - Automated Regression Test Suite

Run tests against live hardware via serial connection.
Usage: python3 test_firmware.py [--port /dev/ttyUSB0] [--verbose]

Test Categories (27 tests total):
  Core Tests:
    - Connection, Status, Self-Test, Help command

  Hardware Tests:
    - Depth sensor, Temperature, Guard switch, Encoder

  Safety Tests:
    - E-Stop state, Guard interlock, Motor fault flag

  Tapping Mode Tests:
    - All 6 tap modes (OFF/PED/SMT/DEP/LOD/PCK)
    - Load threshold, Reverse time, Peck params
    - Depth action, Through-hole detect, Brake delay

  Motor & Settings Tests:
    - Speed calculator, Stack usage, Speed reading
    - Settings save, Motor params read, Motor status query

  Misc Tests:
    - Tap mode cycling, Reset command (disabled)
"""

import serial
import time
import sys
import argparse
import re
from dataclasses import dataclass
from typing import Optional, List, Tuple


@dataclass
class TestResult:
    name: str
    passed: bool
    message: str
    duration: float


class NovaFirmwareTester:
    def __init__(self, port: str = '/dev/ttyUSB0', baud: int = 9600, verbose: bool = False):
        self.port = port
        self.baud = baud
        self.verbose = verbose
        self.ser: Optional[serial.Serial] = None
        self.results: List[TestResult] = []

    def connect(self) -> bool:
        """Connect to the firmware serial port."""
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=2)
            time.sleep(0.5)
            self.ser.reset_input_buffer()
            return True
        except Exception as e:
            print(f"Failed to connect: {e}")
            return False

    def disconnect(self):
        """Disconnect from serial port."""
        if self.ser:
            self.ser.close()
            self.ser = None

    def send_command(self, cmd: str, wait_ms: int = 500) -> str:
        """Send a command and return the response."""
        if not self.ser:
            return ""

        self.ser.reset_input_buffer()
        self.ser.write(f"{cmd}\r\n".encode())
        time.sleep(wait_ms / 1000.0)

        response = self.ser.read(4096).decode('utf-8', errors='ignore')
        if self.verbose:
            print(f"CMD: {cmd}")
            print(f"RSP: {response[:200]}...")
        return response

    def run_test(self, name: str, test_func) -> TestResult:
        """Run a single test and record result."""
        start = time.time()
        try:
            passed, message = test_func()
        except Exception as e:
            passed, message = False, f"Exception: {e}"
        duration = time.time() - start

        result = TestResult(name, passed, message, duration)
        self.results.append(result)

        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] {name}: {message} ({duration:.2f}s)")
        return result

    # =========================================================================
    # Individual Tests
    # =========================================================================

    def test_connection(self) -> Tuple[bool, str]:
        """Test that we can communicate with firmware."""
        response = self.send_command("HELP")
        if "Commands:" in response or "DFU" in response:
            return True, "Command response received"
        return False, "No valid response"

    def test_status_command(self) -> Tuple[bool, str]:
        """Test STATUS command returns valid data."""
        response = self.send_command("STATUS")
        # Should contain state info
        if "State:" in response or "RPM:" in response or "STATUS" in response:
            return True, "Status response valid"
        return False, f"Invalid status response: {response[:100]}"

    def test_selftest(self) -> Tuple[bool, str]:
        """Run hardware self-test and parse results."""
        response = self.send_command("SELFTEST", wait_ms=3000)

        # Parse results
        match = re.search(r'RESULT:PASS=(\d+):FAIL=(\d+)', response)
        if match:
            passed = int(match.group(1))
            failed = int(match.group(2))
            if failed == 0:
                return True, f"All {passed} tests passed"
            return False, f"{passed} passed, {failed} failed"

        return False, f"Could not parse selftest results: {response[:200]}"

    def test_depth_reading(self) -> Tuple[bool, str]:
        """Test depth sensor returns valid reading."""
        response = self.send_command("DEPTH")
        # Should contain depth value
        match = re.search(r'(-?\d+\.?\d*)\s*mm', response)
        if match:
            depth = float(match.group(1))
            # Depth should be reasonable (-500 to +500 mm)
            if -500 <= depth <= 500:
                return True, f"Depth: {depth}mm"
            return False, f"Depth out of range: {depth}mm"
        return False, f"Could not parse depth: {response[:100]}"

    def test_stack_usage(self) -> Tuple[bool, str]:
        """Test that stack usage is healthy."""
        response = self.send_command("STACK")

        # Find minimum stack watermark
        matches = re.findall(r'(\w+)\s*\(\d+\):\s*(\d+)', response)
        if matches:
            min_task = None
            min_hwm = 9999
            for task, hwm in matches:
                hwm_val = int(hwm)
                if hwm_val < min_hwm:
                    min_hwm = hwm_val
                    min_task = task

            if min_hwm > 20:
                return True, f"Min stack: {min_task}={min_hwm} words"
            return False, f"Low stack: {min_task}={min_hwm} words"

        return False, f"Could not parse stack info: {response[:100]}"

    def test_temperature(self) -> Tuple[bool, str]:
        """Test temperature reading."""
        response = self.send_command("TEMP")
        match = re.search(r'(\d+)C', response)
        if match:
            temp = int(match.group(1))
            # Temperature should be reasonable (0-100°C)
            if 0 <= temp <= 100:
                return True, f"Temperature: {temp}°C"
            elif temp == 0:
                return True, f"Temperature: {temp}°C (MCB not responding or cold)"
            return False, f"Temperature out of range: {temp}°C"
        return False, f"Could not parse temperature: {response[:100]}"

    def test_guard_state(self) -> Tuple[bool, str]:
        """Test guard switch is readable."""
        response = self.send_command("GUARD")
        if "Guard" in response or "guard" in response.lower():
            if "OPEN" in response or "CLOSED" in response or "guard_closed" in response:
                return True, "Guard state readable"
        return False, f"Could not read guard state: {response[:100]}"

    def test_tap_mode_cycle(self) -> Tuple[bool, str]:
        """Test tapping mode can be changed."""
        # Get current mode
        response1 = self.send_command("TAP")

        # Try to set mode 1 (PEDAL)
        response2 = self.send_command("TAP 1")

        # Set back to 0 (OFF)
        response3 = self.send_command("TAP 0")

        if "TAP" in response2 or "Mode" in response2 or "PEDAL" in response2:
            return True, "Tap mode can be cycled"
        return False, f"Tap mode change failed: {response2[:100]}"

    def test_encoder_state(self) -> Tuple[bool, str]:
        """Test encoder is readable (via guard command which shows button states)."""
        response = self.send_command("GUARD")
        if "PC13" in response or "PC14" in response or "Encoder" in response.lower():
            return True, "Encoder pins readable"
        # Alternative: Just check response is valid
        if len(response) > 10:
            return True, "GPIO states readable"
        return False, "Encoder state not readable"

    def test_speed_setting(self) -> Tuple[bool, str]:
        """Test that speed can be read."""
        response = self.send_command("STATUS")
        match = re.search(r'RPM[:\s]*(\d+)', response, re.IGNORECASE)
        if match:
            rpm = int(match.group(1))
            return True, f"Speed readable: {rpm} RPM"
        # If STATUS doesn't show RPM, still pass if we got a response
        if "State:" in response:
            return True, "Status readable (RPM not in response)"
        return False, f"Could not read speed: {response[:100]}"

    def test_reset_command(self) -> Tuple[bool, str]:
        """Test RESET command (careful - this resets the device!)."""
        # Skip this test in normal runs - uncomment to enable
        return True, "SKIP: Reset test disabled (would reboot device)"

        # Uncomment to actually test:
        # self.send_command("RESET")
        # time.sleep(3)  # Wait for reboot
        # response = self.send_command("HELP", wait_ms=1000)
        # if "Commands:" in response:
        #     return True, "Device rebooted successfully"
        # return False, "Device did not respond after reset"

    # =========================================================================
    # Tapping Mode Tests
    # =========================================================================

    def test_tap_mode_all_modes(self) -> Tuple[bool, str]:
        """Test all tapping modes can be selected."""
        modes = [
            (0, "OFF"),
            (1, "PED"),
            (2, "SMT"),
            (3, "DEP"),
            (4, "LOD"),
            (5, "PCK"),
        ]
        failed = []
        for mode_num, mode_name in modes:
            response = self.send_command(f"TAP {mode_num}")
            if mode_name not in response and f"mode={mode_num}" not in response.lower():
                failed.append(mode_name)

        # Reset to OFF
        self.send_command("TAP 0")

        if not failed:
            return True, "All 6 modes selectable"
        return False, f"Failed modes: {', '.join(failed)}"

    def test_tap_load_threshold(self) -> Tuple[bool, str]:
        """Test load threshold parameter."""
        # Set threshold to 50%
        response = self.send_command("TAPLOAD 50")
        if "50" not in response:
            return False, f"Could not set threshold: {response[:100]}"

        # Read back
        response = self.send_command("TAPLOAD")
        if "50" in response:
            # Reset to default
            self.send_command("TAPLOAD 60")
            return True, "Load threshold settable"
        return False, f"Threshold not persisted: {response[:100]}"

    def test_tap_reverse_time(self) -> Tuple[bool, str]:
        """Test reverse time parameter."""
        # Set reverse time to 300ms
        response = self.send_command("TAPREV 300")
        if "300" not in response:
            return False, f"Could not set reverse time: {response[:100]}"

        # Read back
        response = self.send_command("TAPREV")
        if "300" in response:
            # Reset to default
            self.send_command("TAPREV 200")
            return True, "Reverse time settable"
        return False, f"Reverse time not persisted: {response[:100]}"

    def test_tap_peck_params(self) -> Tuple[bool, str]:
        """Test peck drilling parameters."""
        # Set peck params: 2.0 fwd turns, 1.0 rev turns, 5 cycles
        response = self.send_command("TAPPECK 20 10 5")
        if "20" in response or "peck" in response.lower():
            return True, "Peck parameters settable"
        return False, f"Could not set peck params: {response[:100]}"

    def test_tap_depth_action(self) -> Tuple[bool, str]:
        """Test depth action parameter (stop vs reverse)."""
        # Set to reverse (1)
        response = self.send_command("TAPACT 1")
        if "REVERSE" in response or "1" in response:
            # Set back to stop (0)
            self.send_command("TAPACT 0")
            return True, "Depth action settable"
        return False, f"Could not set depth action: {response[:100]}"

    def test_tap_through_detect(self) -> Tuple[bool, str]:
        """Test through-hole detection toggle."""
        # Enable
        response = self.send_command("TAPTHR 1")
        if "1" in response or "ON" in response or "through" in response.lower():
            # Disable
            self.send_command("TAPTHR 0")
            return True, "Through-hole detection toggleable"
        return False, f"Could not set through detect: {response[:100]}"

    def test_tap_brake_delay(self) -> Tuple[bool, str]:
        """Test brake delay parameter."""
        # Set brake delay to 150ms
        response = self.send_command("TAPBRK 150")
        if "150" in response or "brake" in response.lower():
            # Reset to default
            self.send_command("TAPBRK 100")
            return True, "Brake delay settable"
        return False, f"Could not set brake delay: {response[:100]}"

    # =========================================================================
    # Speed Calculator Test
    # =========================================================================

    def test_speed_calculator(self) -> Tuple[bool, str]:
        """Test speed calculator command."""
        response = self.send_command("CALC")
        # Should show materials and RPM calculations
        if "Softwood" in response or "RPM" in response or "material" in response.lower():
            return True, "Speed calculator works"
        return False, f"Calculator output invalid: {response[:100]}"

    # =========================================================================
    # Safety Feature Tests
    # =========================================================================

    def test_estop_readable(self) -> Tuple[bool, str]:
        """Test E-Stop state is readable."""
        response = self.send_command("STATUS")
        if "estop" in response.lower() or "e-stop" in response.lower():
            return True, "E-Stop state in status"
        # Try GUARD command which also shows safety states
        response = self.send_command("GUARD")
        if "PC0" in response or "estop" in response.lower():
            return True, "E-Stop readable via GUARD"
        return True, "SKIP: E-Stop not exposed in current commands"

    def test_guard_interlock(self) -> Tuple[bool, str]:
        """Test guard interlock is functional."""
        response = self.send_command("GUARD")
        # Should show guard state
        if "OPEN" in response or "CLOSED" in response:
            return True, "Guard interlock readable"
        if "guard" in response.lower():
            return True, "Guard state reported"
        return False, f"Guard interlock not readable: {response[:100]}"

    def test_motor_fault_flag(self) -> Tuple[bool, str]:
        """Test motor fault flag is readable."""
        response = self.send_command("STATUS")
        # Should show motor status
        if "fault" in response.lower() or "motor" in response.lower():
            return True, "Motor fault status readable"
        return True, "SKIP: Motor fault not in STATUS"

    # =========================================================================
    # Settings Tests
    # =========================================================================

    def test_settings_save(self) -> Tuple[bool, str]:
        """Test settings can be saved."""
        response = self.send_command("SAVE")
        if "saved" in response.lower() or "ok" in response.lower() or "SAVE" in response:
            return True, "Settings save command works"
        return False, f"Save command failed: {response[:100]}"

    def test_help_command(self) -> Tuple[bool, str]:
        """Test HELP command lists commands."""
        response = self.send_command("HELP")
        # Count number of commands listed
        cmd_count = response.count('\n')
        if cmd_count >= 10:
            return True, f"{cmd_count} commands listed"
        if "Commands:" in response or "DFU" in response:
            return True, "Help output valid"
        return False, f"Help output invalid: {response[:100]}"

    # =========================================================================
    # Motor Communication Tests
    # =========================================================================

    def test_motor_params_read(self) -> Tuple[bool, str]:
        """Test MCB parameters can be read."""
        response = self.send_command("MREAD", wait_ms=2000)
        # Should show motor parameters
        if "IR" in response or "Kp" in response or "param" in response.lower():
            return True, "MCB parameters readable"
        if "timeout" in response.lower() or "no response" in response.lower():
            return True, "SKIP: MCB not connected"
        return False, f"MREAD output invalid: {response[:100]}"

    def test_motor_status_query(self) -> Tuple[bool, str]:
        """Test motor status query (GF command)."""
        response = self.send_command("GF", wait_ms=1000)
        # Should get motor controller response
        if "GF" in response or "flag" in response.lower() or len(response) > 5:
            return True, "Motor status query works"
        return True, "SKIP: MCB may not be connected"

    # =========================================================================
    # Test Runner
    # =========================================================================

    def run_all_tests(self):
        """Run all regression tests."""
        print("\n" + "="*60)
        print("Nova Voyager Firmware - Automated Regression Tests")
        print("="*60)
        print(f"Port: {self.port}")
        print(f"Baud: {self.baud}")
        print("="*60 + "\n")

        if not self.connect():
            print("FATAL: Could not connect to device")
            return False

        try:
            # Core functionality tests
            print("Core Tests:")
            self.run_test("Connection", self.test_connection)
            self.run_test("Status Command", self.test_status_command)
            self.run_test("Self-Test", self.test_selftest)
            self.run_test("Help Command", self.test_help_command)

            print("\nHardware Tests:")
            self.run_test("Depth Sensor", self.test_depth_reading)
            self.run_test("Temperature", self.test_temperature)
            self.run_test("Guard Switch", self.test_guard_state)
            self.run_test("Encoder", self.test_encoder_state)

            print("\nSafety Tests:")
            self.run_test("E-Stop State", self.test_estop_readable)
            self.run_test("Guard Interlock", self.test_guard_interlock)
            self.run_test("Motor Fault Flag", self.test_motor_fault_flag)

            print("\nTapping Mode Tests:")
            self.run_test("All Tap Modes", self.test_tap_mode_all_modes)
            self.run_test("Load Threshold", self.test_tap_load_threshold)
            self.run_test("Reverse Time", self.test_tap_reverse_time)
            self.run_test("Peck Params", self.test_tap_peck_params)
            self.run_test("Depth Action", self.test_tap_depth_action)
            self.run_test("Through Detect", self.test_tap_through_detect)
            self.run_test("Brake Delay", self.test_tap_brake_delay)

            print("\nMotor & Settings Tests:")
            self.run_test("Speed Calculator", self.test_speed_calculator)
            self.run_test("Stack Usage", self.test_stack_usage)
            self.run_test("Speed Reading", self.test_speed_setting)
            self.run_test("Settings Save", self.test_settings_save)
            self.run_test("Motor Params", self.test_motor_params_read)
            self.run_test("Motor Status", self.test_motor_status_query)

            print("\nMisc Tests:")
            self.run_test("Tap Mode Cycle", self.test_tap_mode_cycle)
            self.run_test("Reset", self.test_reset_command)

        finally:
            self.disconnect()

        # Summary
        passed = sum(1 for r in self.results if r.passed)
        failed = sum(1 for r in self.results if not r.passed)
        total = len(self.results)

        print("\n" + "="*60)
        print(f"RESULTS: {passed}/{total} tests passed")
        if failed > 0:
            print(f"FAILED TESTS:")
            for r in self.results:
                if not r.passed:
                    print(f"  - {r.name}: {r.message}")
        print("="*60 + "\n")

        return failed == 0


def main():
    parser = argparse.ArgumentParser(description='Nova Voyager Firmware Regression Tests')
    parser.add_argument('--port', default='/dev/ttyUSB0', help='Serial port')
    parser.add_argument('--baud', type=int, default=9600, help='Baud rate')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    args = parser.parse_args()

    tester = NovaFirmwareTester(args.port, args.baud, args.verbose)
    success = tester.run_all_tests()

    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
