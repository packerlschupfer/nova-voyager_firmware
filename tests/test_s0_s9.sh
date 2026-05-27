#!/bin/bash
# Test S0-S9 profile commands interactively
# Run this with: ssh pi@192.168.16.62 "~/test_s0_s9.sh"

echo "=== Nova Voyager S0-S9 Profile Discovery ==="
echo
echo "Instructions:"
echo "1. Make sure motor is STOPPED"
echo "2. Watch the output carefully"
echo "3. Press Ctrl+C when done"
echo
echo "Starting test in 3 seconds..."
sleep 3

# Test each S0-S9 command
for i in $(seq 0 9); do
    echo
    echo ">>> Testing S$i (query format)..."
    echo "QQ S$i" > /dev/ttyUSB0
    sleep 1
done

echo
echo ">>> Testing HT (thermal query)..."
echo "QQ HT" > /dev/ttyUSB0
sleep 1

echo
echo ">>> Testing VG (vibration gain query)..."
echo "QQ VG" > /dev/ttyUSB0
sleep 1

echo
echo "=== Test Complete ==="
echo "Check the serial output above for responses:"
echo "  - 06 = ACK (command accepted)"
echo "  - 15 = NAK (command rejected)"
echo "  - 02 ... 03 = Data response"
echo "  - timeout = Command not recognized"
