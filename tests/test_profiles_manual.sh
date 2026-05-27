#!/bin/bash
# Manual profile testing - watch the LCD and listen to motor
# This script sends commands and you observe the behavior

echo "======================================================================"
echo "MOTOR PROFILE BEHAVIOR TEST - MANUAL"
echo "======================================================================"
echo
echo "This test will:"
echo "  1. Start motor at 1000 RPM"
echo "  2. Switch between S0 (SOFT), S7 (NORMAL), S8 (HARD)"
echo "  3. You observe motor sound/behavior on each profile"
echo
echo "SAFETY:"
echo "  - Guard CLOSED (confirmed)"
echo "  - E-stop within reach"
echo "  - Watch motor for 10 seconds on each profile"
echo
echo "OBSERVATION:"
echo "  - Does motor sound change?"
echo "  - Does torque response feel different?"
echo "  - Does speed stability change?"
echo
read -p "Press Enter to start (Ctrl+C to cancel)..."

DEV=/dev/ttyUSB0

echo
echo "======================================================================="
echo "Starting motor at 1000 RPM..."
echo "======================================================================="
echo "SPEED 1000" > $DEV
sleep 0.5
echo "START" > $DEV
sleep 3

echo
echo "Motor should be running now. Waiting 5 seconds for stabilization..."
sleep 5

echo
echo "======================================================================="
echo "TEST 1: Setting S0 (SOFT profile)"
echo "======================================================================="
echo "  Sending S0(0) command..."
# Use Python to send properly formatted command
python3 <<EOF
import serial, time
ser = serial.Serial('$DEV', 9600, timeout=1)
pkt = [0x04, 0x30, 0x30, 0x31, 0x31, 0x02, 0x31, ord('S'), ord('0'), ord('0'), 0x03]
xor = 0
for i in range(6, len(pkt)): xor ^= pkt[i]
pkt.append(xor)
ser.write(bytes(pkt))
ser.close()
EOF

echo "  OBSERVE MOTOR FOR 10 SECONDS"
echo "  - How does it sound?"
echo "  - Try gently touching the chuck - does it resist?"
for i in {1..10}; do
    echo "    [$i/10]"
    sleep 1
done

echo
echo "======================================================================="
echo "TEST 2: Setting S7 (NORMAL profile)"
echo "======================================================================="
echo "  Sending S7(750) command..."
python3 <<EOF
import serial, time
ser = serial.Serial('$DEV', 9600, timeout=1)
pkt = [0x04, 0x30, 0x30, 0x31, 0x31, 0x02, 0x31, ord('S'), ord('7')]
pkt.extend([ord('7'), ord('5'), ord('0')])
pkt.append(0x03)
xor = 0
for i in range(6, len(pkt)): xor ^= pkt[i]
pkt.append(xor)
ser.write(bytes(pkt))
ser.close()
EOF

echo "  OBSERVE MOTOR FOR 10 SECONDS"
echo "  - Does it sound different from S0?"
echo "  - Different torque response?"
for i in {1..10}; do
    echo "    [$i/10]"
    sleep 1
done

echo
echo "======================================================================="
echo "TEST 3: Setting S8 (HARD profile)"
echo "======================================================================="
echo "  Sending S8(264) command..."
python3 <<EOF
import serial, time
ser = serial.Serial('$DEV', 9600, timeout=1)
pkt = [0x04, 0x30, 0x30, 0x31, 0x31, 0x02, 0x31, ord('S'), ord('8')]
pkt.extend([ord('2'), ord('6'), ord('4')])
pkt.append(0x03)
xor = 0
for i in range(6, len(pkt)): xor ^= pkt[i]
pkt.append(xor)
ser.write(bytes(pkt))
ser.close()
EOF

echo "  OBSERVE MOTOR FOR 10 SECONDS"
echo "  - Most aggressive profile - different?"
echo "  - Stronger torque response?"
for i in {1..10}; do
    echo "    [$i/10]"
    sleep 1
done

echo
echo "======================================================================="
echo "Stopping motor..."
echo "======================================================================="
echo "STOP" > $DEV
sleep 2

echo
echo "======================================================================"
echo "TEST COMPLETE"
echo "======================================================================"
echo
echo "QUESTIONS:"
echo "  1. Did you hear any difference in motor sound between profiles?"
echo "  2. Did torque response feel different when touching chuck?"
echo "  3. Did speed seem more/less stable on different profiles?"
echo
echo "If YES to any: Profile commands DO affect motor behavior!"
echo "If NO to all:  Profiles may be status registers only"
echo
echo "Results?"
read -p "Did you notice ANY difference? (y/n): " RESULT

if [ "$RESULT" = "y" ] || [ "$RESULT" = "Y" ]; then
    echo
    echo "CONCLUSION: Profiles affect motor! Implement profile menu."
else
    echo
    echo "CONCLUSION: Profiles don't affect behavior (read-only registers?)"
fi
