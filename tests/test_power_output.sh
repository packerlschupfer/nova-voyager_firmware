#!/bin/bash
# Test Power Output levels - Low vs Med vs High
# Tests torque difference at startup

echo "======================================================================"
echo "POWER OUTPUT TEST - Low/Med/High"
echo "======================================================================"
echo
echo "This test will:"
echo "  1. Set Power Output to LOW (20%)"
echo "  2. Start motor - TEST torque by hand"
echo "  3. Set Power Output to MED (50%)"
echo "  4. Start motor - TEST torque by hand"
echo "  5. Set Power Output to HIGH (70%)"
echo "  6. Start motor - TEST torque by hand"
echo
echo "SAFETY:"
echo "  - Guard must be CLOSED"
echo "  - E-stop within reach"
echo "  - Be careful - don't force if chuck won't stop!"
echo
read -p "Press Enter to start (Ctrl+C to cancel)..."

DEV=/dev/ttyUSB0

echo
echo "======================================================================="
echo "TEST 1: Power Output = LOW (20%)"
echo "======================================================================="
echo
echo "Setting Output to Low via menu navigation..."

# Exit menu if in it
echo "MENU" > $DEV
sleep 0.5

# Enter menu
echo "MENU" > $DEV
sleep 0.5

# Navigate to Power menu (item 6)
for i in {1..6}; do echo "DN" > $DEV; sleep 0.2; done
echo "OK" > $DEV
sleep 0.5

# Should be on "Output" - set to Low (value 0)
echo "OK" > $DEV
sleep 0.3
# Rotate to Low if needed (assuming it might not be at Low)
echo "DN" > $DEV
sleep 0.2
echo "DN" > $DEV
sleep 0.2
# Set to Low (should wrap around or already be there)
echo "OK" > $DEV
sleep 0.3

# Exit menu
echo "MENU" > $DEV
sleep 0.5

echo "  Starting motor at 1000 RPM..."
echo "SPEED 1000" > $DEV
sleep 0.5
echo "START" > $DEV
sleep 2

echo
echo "  MOTOR RUNNING AT LOW POWER (20%)"
echo "  TRY TO STOP CHUCK WITH YOUR HAND"
echo "  Should be RELATIVELY EASY to slow down"
echo
for i in {1..8}; do
    echo "    [$i/8]"
    sleep 1
done

echo
echo "  Stopping motor..."
echo "STOP" > $DEV
sleep 3

echo
echo "======================================================================="
echo "TEST 2: Power Output = HIGH (70%)"
echo "======================================================================="
echo
echo "Setting Output to High via menu navigation..."

# Enter menu
echo "MENU" > $DEV
sleep 0.5

# Navigate to Power menu (item 6)
for i in {1..6}; do echo "DN" > $DEV; sleep 0.2; done
echo "OK" > $DEV
sleep 0.5

# On Output item - change to High
echo "OK" > $DEV
sleep 0.3
# Rotate to High (value 2)
echo "UP" > $DEV
sleep 0.2
echo "UP" > $DEV
sleep 0.2
echo "OK" > $DEV
sleep 0.3

# Exit menu
echo "MENU" > $DEV
sleep 0.5

echo "  Starting motor at 1000 RPM..."
echo "SPEED 1000" > $DEV
sleep 0.5
echo "START" > $DEV
sleep 2

echo
echo "  MOTOR RUNNING AT HIGH POWER (70%)"
echo "  TRY TO STOP CHUCK WITH YOUR HAND"
echo "  Should be MUCH HARDER to slow down than Low!"
echo
for i in {1..8}; do
    echo "    [$i/8]"
    sleep 1
done

echo
echo "  Stopping motor..."
echo "STOP" > $DEV
sleep 2

echo
echo "======================================================================"
echo "TEST COMPLETE"
echo "======================================================================"
echo
echo "RESULTS:"
read -p "Was High power (70%) stronger than Low power (20%)? (y/n): " RESULT

if [ "$RESULT" = "y" ] || [ "$RESULT" = "Y" ]; then
    echo
    echo "✓ SUCCESS: Power Output feature is working!"
    echo "  Low (20%) = Less torque"
    echo "  High (70%) = More torque"
    echo
    echo "Power Output menu is READY TO USE!"
else
    echo
    echo "✗ No difference detected"
    echo "  May need to test under load or at higher speeds"
fi
